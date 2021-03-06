#include <vcpkg/base/json.h>
#include <vcpkg/base/system.debug.h>

#include <vcpkg/configuration.h>
#include <vcpkg/paragraphs.h>
#include <vcpkg/portfileprovider.h>
#include <vcpkg/registries.h>
#include <vcpkg/sourceparagraph.h>
#include <vcpkg/vcpkgcmdarguments.h>
#include <vcpkg/vcpkgpaths.h>
#include <vcpkg/versiondeserializers.h>

#include <regex>

using namespace vcpkg;
using namespace Versions;

namespace
{
    ExpectedS<fs::path> get_versions_json_path(const VcpkgPaths& paths, StringView port_name)
    {
        auto json_path = paths.root / fs::u8path("port_versions") /
                         fs::u8path(Strings::concat(port_name.substr(0, 1), "-")) /
                         fs::u8path(Strings::concat(port_name, ".json"));
        if (paths.get_filesystem().exists(json_path))
        {
            return std::move(json_path);
        }
        return {Strings::concat("Error: Versions database file does not exist: ", fs::u8string(json_path)),
                expected_right_tag};
    }

    ExpectedS<fs::path> get_baseline_json_path(const VcpkgPaths& paths, StringView baseline_commit_sha)
    {
        auto baseline_path = paths.git_checkout_baseline(paths.get_filesystem(), baseline_commit_sha);
        if (paths.get_filesystem().exists(baseline_path))
        {
            return std::move(baseline_path);
        }
        return {Strings::concat("Error: Baseline database file does not exist: ", fs::u8string(baseline_path)),
                expected_right_tag};
    }
}

namespace vcpkg::PortFileProvider
{
    MapPortFileProvider::MapPortFileProvider(const std::unordered_map<std::string, SourceControlFileLocation>& map)
        : ports(map)
    {
    }

    ExpectedS<const SourceControlFileLocation&> MapPortFileProvider::get_control_file(const std::string& spec) const
    {
        auto scf = ports.find(spec);
        if (scf == ports.end()) return std::string("does not exist in map");
        return scf->second;
    }

    std::vector<const SourceControlFileLocation*> MapPortFileProvider::load_all_control_files() const
    {
        return Util::fmap(ports, [](auto&& kvpair) -> const SourceControlFileLocation* { return &kvpair.second; });
    }

    PathsPortFileProvider::PathsPortFileProvider(const VcpkgPaths& paths_,
                                                 const std::vector<std::string>& overlay_ports_)
        : paths(paths_)
    {
        auto& fs = paths.get_filesystem();
        for (auto&& overlay_path : overlay_ports_)
        {
            if (!overlay_path.empty())
            {
                auto overlay = fs::u8path(overlay_path);
                if (overlay.is_absolute())
                {
                    overlay = fs.canonical(VCPKG_LINE_INFO, overlay);
                }
                else
                {
                    overlay = fs.canonical(VCPKG_LINE_INFO, paths.original_cwd / overlay);
                }

                Debug::print("Using overlay: ", fs::u8string(overlay), "\n");

                Checks::check_exit(
                    VCPKG_LINE_INFO, fs.exists(overlay), "Error: Path \"%s\" does not exist", fs::u8string(overlay));

                Checks::check_exit(VCPKG_LINE_INFO,
                                   fs::is_directory(fs.status(VCPKG_LINE_INFO, overlay)),
                                   "Error: Path \"%s\" must be a directory",
                                   overlay.string());

                overlay_ports.emplace_back(overlay);
            }
        }
    }

    static Optional<SourceControlFileLocation> try_load_overlay_port(const Files::Filesystem& fs,
                                                                     View<fs::path> overlay_ports,
                                                                     const std::string& spec)
    {
        for (auto&& ports_dir : overlay_ports)
        {
            // Try loading individual port
            if (Paragraphs::is_port_directory(fs, ports_dir))
            {
                auto maybe_scf = Paragraphs::try_load_port(fs, ports_dir);
                if (auto scf = maybe_scf.get())
                {
                    if (scf->get()->core_paragraph->name == spec)
                    {
                        return SourceControlFileLocation{std::move(*scf), ports_dir};
                    }
                }
                else
                {
                    print_error_message(maybe_scf.error());
                    Checks::exit_with_message(
                        VCPKG_LINE_INFO, "Error: Failed to load port %s from %s", spec, fs::u8string(ports_dir));
                }

                continue;
            }

            auto ports_spec = ports_dir / fs::u8path(spec);
            if (Paragraphs::is_port_directory(fs, ports_spec))
            {
                auto found_scf = Paragraphs::try_load_port(fs, ports_spec);
                if (auto scf = found_scf.get())
                {
                    if (scf->get()->core_paragraph->name == spec)
                    {
                        return SourceControlFileLocation{std::move(*scf), std::move(ports_spec)};
                    }
                    Checks::exit_with_message(VCPKG_LINE_INFO,
                                              "Error: Failed to load port from %s: names did not match: '%s' != '%s'",
                                              fs::u8string(ports_spec),
                                              spec,
                                              scf->get()->core_paragraph->name);
                }
                else
                {
                    print_error_message(found_scf.error());
                    Checks::exit_with_message(
                        VCPKG_LINE_INFO, "Error: Failed to load port %s from %s", spec, fs::u8string(ports_dir));
                }
            }
        }
        return nullopt;
    }

    static Optional<SourceControlFileLocation> try_load_registry_port(const VcpkgPaths& paths, const std::string& spec)
    {
        const auto& fs = paths.get_filesystem();
        if (auto registry = paths.get_configuration().registry_set.registry_for_port(spec))
        {
            auto baseline_version = registry->get_baseline_version(paths, spec);
            auto entry = registry->get_port_entry(paths, spec);
            if (entry && baseline_version)
            {
                auto port_directory = entry->get_port_directory(paths, *baseline_version.get());
                if (port_directory.empty())
                {
                    Checks::exit_with_message(VCPKG_LINE_INFO,
                                              "Error: registry is incorrect. Baseline version for port `%s` is `%s`, "
                                              "but that version is not in the registry.\n",
                                              spec,
                                              baseline_version.get()->to_string());
                }
                auto found_scf = Paragraphs::try_load_port(fs, port_directory);
                if (auto scf = found_scf.get())
                {
                    if (scf->get()->core_paragraph->name == spec)
                    {
                        return SourceControlFileLocation{std::move(*scf), std::move(port_directory)};
                    }
                    Checks::exit_with_message(VCPKG_LINE_INFO,
                                              "Error: Failed to load port from %s: names did not match: '%s' != '%s'",
                                              fs::u8string(port_directory),
                                              spec,
                                              scf->get()->core_paragraph->name);
                }
                else
                {
                    print_error_message(found_scf.error());
                    Checks::exit_with_message(
                        VCPKG_LINE_INFO, "Error: Failed to load port %s from %s", spec, fs::u8string(port_directory));
                }
            }
            else
            {
                Debug::print("Failed to find port `",
                             spec,
                             "` in registry:",
                             entry ? " entry found;" : " no entry found;",
                             baseline_version ? " baseline version found\n" : " no baseline version found\n");
            }
        }
        else
        {
            Debug::print("Failed to find registry for port: `", spec, "`.\n");
        }
        return nullopt;
    }

    ExpectedS<const SourceControlFileLocation&> PathsPortFileProvider::get_control_file(const std::string& spec) const
    {
        auto cache_it = cache.find(spec);
        if (cache_it == cache.end())
        {
            const auto& fs = paths.get_filesystem();
            auto maybe_port = try_load_overlay_port(fs, overlay_ports, spec);
            if (!maybe_port)
            {
                maybe_port = try_load_registry_port(paths, spec);
            }
            if (auto p = maybe_port.get())
            {
                auto maybe_error =
                    p->source_control_file->check_against_feature_flags(p->source_location, paths.get_feature_flags());
                if (maybe_error) return std::move(*maybe_error.get());

                cache_it = cache.emplace(spec, std::move(*p)).first;
            }
        }

        if (cache_it == cache.end())
        {
            return std::string("Port definition not found");
        }
        else
        {
            return cache_it->second;
        }
    }

    std::vector<const SourceControlFileLocation*> PathsPortFileProvider::load_all_control_files() const
    {
        // Reload cache with ports contained in all ports_dirs
        cache.clear();
        std::vector<const SourceControlFileLocation*> ret;

        for (const fs::path& ports_dir : overlay_ports)
        {
            // Try loading individual port
            if (Paragraphs::is_port_directory(paths.get_filesystem(), ports_dir))
            {
                auto maybe_scf = Paragraphs::try_load_port(paths.get_filesystem(), ports_dir);
                if (auto scf = maybe_scf.get())
                {
                    auto port_name = scf->get()->core_paragraph->name;
                    if (cache.find(port_name) == cache.end())
                    {
                        auto scfl = SourceControlFileLocation{std::move(*scf), ports_dir};
                        auto it = cache.emplace(std::move(port_name), std::move(scfl));
                        ret.emplace_back(&it.first->second);
                    }
                }
                else
                {
                    print_error_message(maybe_scf.error());
                    Checks::exit_with_message(
                        VCPKG_LINE_INFO, "Error: Failed to load port from %s", fs::u8string(ports_dir));
                }
                continue;
            }

            // Try loading all ports inside ports_dir
            auto found_scfls = Paragraphs::load_overlay_ports(paths, ports_dir);
            for (auto&& scfl : found_scfls)
            {
                auto port_name = scfl.source_control_file->core_paragraph->name;
                if (cache.find(port_name) == cache.end())
                {
                    auto it = cache.emplace(std::move(port_name), std::move(scfl));
                    ret.emplace_back(&it.first->second);
                }
            }
        }

        auto all_ports = Paragraphs::load_all_registry_ports(paths);
        for (auto&& scfl : all_ports)
        {
            auto port_name = scfl.source_control_file->core_paragraph->name;
            if (cache.find(port_name) == cache.end())
            {
                auto it = cache.emplace(port_name, std::move(scfl));
                ret.emplace_back(&it.first->second);
            }
        }

        return ret;
    }

    namespace
    {
        struct BaselineProviderImpl : IBaselineProvider, Util::ResourceBase
        {
            BaselineProviderImpl(const VcpkgPaths& paths) : paths(paths) { }
            BaselineProviderImpl(const VcpkgPaths& paths, StringView baseline)
                : paths(paths), m_baseline(baseline.to_string())
            {
            }

            const Optional<std::map<std::string, VersionT, std::less<>>>& get_baseline_cache() const
            {
                return baseline_cache.get_lazy([&]() -> Optional<std::map<std::string, VersionT, std::less<>>> {
                    if (auto baseline = m_baseline.get())
                    {
                        auto baseline_file = get_baseline_json_path(paths, *baseline).value_or_exit(VCPKG_LINE_INFO);

                        auto maybe_baselines_map =
                            parse_baseline_file(paths.get_filesystem(), "default", baseline_file);
                        Checks::check_exit(VCPKG_LINE_INFO,
                                           maybe_baselines_map.has_value(),
                                           "Error: Couldn't parse baseline `%s` from `%s`",
                                           "default",
                                           fs::u8string(baseline_file));
                        auto baselines_map = *maybe_baselines_map.get();
                        return std::move(baselines_map);
                    }
                    else
                    {
                        // No baseline was provided, so use current repo
                        const auto& fs = paths.get_filesystem();
                        auto baseline_file = paths.root / fs::u8path("port_versions") / fs::u8path("baseline.json");
                        if (fs.exists(baseline_file))
                        {
                            auto maybe_baselines_map =
                                parse_baseline_file(paths.get_filesystem(), "default", baseline_file);
                            Checks::check_exit(VCPKG_LINE_INFO,
                                               maybe_baselines_map.has_value(),
                                               "Error: Couldn't parse baseline `%s` from `%s`",
                                               "default",
                                               fs::u8string(baseline_file));
                            auto baselines_map = *maybe_baselines_map.get();
                            return std::move(baselines_map);
                        }
                        else
                        {
                            // No baseline file in current repo -- use current port versions.
                            m_portfile_provider =
                                std::make_unique<PathsPortFileProvider>(paths, std::vector<std::string>{});
                            return nullopt;
                        }
                    }
                });
            }

            virtual Optional<VersionT> get_baseline_version(StringView port_name) const override
            {
                const auto& cache = get_baseline_cache();
                if (auto p_cache = cache.get())
                {
                    auto it = p_cache->find(port_name.to_string());
                    if (it != p_cache->end())
                    {
                        return it->second;
                    }
                    return nullopt;
                }
                else
                {
                    auto maybe_scfl = m_portfile_provider->get_control_file(port_name.to_string());
                    if (auto p_scfl = maybe_scfl.get())
                    {
                        auto cpgh = p_scfl->source_control_file->core_paragraph.get();
                        return VersionT{cpgh->version, cpgh->port_version};
                    }
                    else
                    {
                        return nullopt;
                    }
                }
            }

        private:
            const VcpkgPaths& paths;
            const Optional<std::string> m_baseline;
            Lazy<Optional<std::map<std::string, VersionT, std::less<>>>> baseline_cache;
            mutable std::unique_ptr<PathsPortFileProvider> m_portfile_provider;
        };

        struct VersionedPortfileProviderImpl : IVersionedPortfileProvider, Util::ResourceBase
        {
            VersionedPortfileProviderImpl(const VcpkgPaths& paths) : paths(paths) { }

            virtual const std::vector<VersionSpec>& get_port_versions(StringView port_name) const override
            {
                auto cache_it = versions_cache.find(port_name.to_string());
                if (cache_it != versions_cache.end())
                {
                    return cache_it->second;
                }

                auto maybe_versions_file_path = get_versions_json_path(get_paths(), port_name);
                if (auto versions_file_path = maybe_versions_file_path.get())
                {
                    auto maybe_version_entries = parse_versions_file(get_filesystem(), port_name, *versions_file_path);
                    Checks::check_exit(VCPKG_LINE_INFO,
                                       maybe_version_entries.has_value(),
                                       "Error: Couldn't parse versions from file: %s",
                                       fs::u8string(*versions_file_path));
                    auto version_entries = maybe_version_entries.value_or_exit(VCPKG_LINE_INFO);

                    auto port = port_name.to_string();
                    for (auto&& version_entry : version_entries)
                    {
                        VersionSpec spec(port, version_entry.version);
                        versions_cache[port].push_back(spec);
                        git_tree_cache.emplace(std::move(spec), std::move(version_entry.git_tree));
                    }
                    return versions_cache.at(port);
                }
                else
                {
                    // Fall back to current available version
                    auto maybe_port = try_load_registry_port(paths, port_name.to_string());
                    if (auto p = maybe_port.get())
                    {
                        auto maybe_error = p->source_control_file->check_against_feature_flags(
                            p->source_location, paths.get_feature_flags());

                        if (auto error = maybe_error.get())
                        {
                            Checks::exit_with_message(VCPKG_LINE_INFO, "Error: %s", *error);
                        }

                        VersionSpec vspec(port_name.to_string(),
                                          VersionT(p->source_control_file->core_paragraph->version,
                                                   p->source_control_file->core_paragraph->port_version));
                        control_cache.emplace(vspec, std::move(*p));
                        return versions_cache.emplace(port_name.to_string(), std::vector<VersionSpec>{vspec})
                            .first->second;
                    }
                    Checks::exit_with_message(
                        VCPKG_LINE_INFO, "Error: Could not find a definition for port %s", port_name);
                }
            }

            virtual ExpectedS<const SourceControlFileLocation&> get_control_file(
                const VersionSpec& version_spec) const override
            {
                // Pre-populate versions cache.
                get_port_versions(version_spec.port_name);

                auto cache_it = control_cache.find(version_spec);
                if (cache_it != control_cache.end())
                {
                    return cache_it->second;
                }

                auto git_tree_cache_it = git_tree_cache.find(version_spec);
                if (git_tree_cache_it == git_tree_cache.end())
                {
                    return Strings::concat("Error: No git object SHA for entry ",
                                           version_spec.port_name,
                                           " at version ",
                                           version_spec.version,
                                           ".");
                }

                const std::string git_tree = git_tree_cache_it->second;
                auto port_directory = get_paths().git_checkout_port(get_filesystem(), version_spec.port_name, git_tree);

                auto maybe_control_file = Paragraphs::try_load_port(get_filesystem(), port_directory);
                if (auto scf = maybe_control_file.get())
                {
                    if (scf->get()->core_paragraph->name == version_spec.port_name)
                    {
                        return control_cache
                            .emplace(version_spec,
                                     SourceControlFileLocation{std::move(*scf), std::move(port_directory)})
                            .first->second;
                    }
                    return Strings::format("Error: Failed to load port from %s: names did not match: '%s' != '%s'",
                                           fs::u8string(port_directory),
                                           version_spec.port_name,
                                           scf->get()->core_paragraph->name);
                }

                print_error_message(maybe_control_file.error());
                return Strings::format(
                    "Error: Failed to load port %s from %s", version_spec.port_name, fs::u8string(port_directory));
            }

            const VcpkgPaths& get_paths() const { return paths; }
            Files::Filesystem& get_filesystem() const { return paths.get_filesystem(); }

        private:
            const VcpkgPaths& paths;
            mutable std::map<std::string, std::vector<VersionSpec>> versions_cache;
            mutable std::unordered_map<VersionSpec, std::string, VersionSpecHasher> git_tree_cache;
            mutable std::unordered_map<VersionSpec, SourceControlFileLocation, VersionSpecHasher> control_cache;
        };
    }

    std::unique_ptr<IBaselineProvider> make_baseline_provider(const vcpkg::VcpkgPaths& paths)
    {
        return std::make_unique<BaselineProviderImpl>(paths);
    }

    std::unique_ptr<IBaselineProvider> make_baseline_provider(const vcpkg::VcpkgPaths& paths, StringView baseline)
    {
        return std::make_unique<BaselineProviderImpl>(paths, baseline);
    }

    std::unique_ptr<IVersionedPortfileProvider> make_versioned_portfile_provider(const vcpkg::VcpkgPaths& paths)
    {
        return std::make_unique<VersionedPortfileProviderImpl>(paths);
    }
}
