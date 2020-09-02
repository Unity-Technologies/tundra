using Bee.BuildTools;
using Bee.Core;
using Bee.NativeProgramSupport;
using Bee.ProjectGeneration.VisualStudio;
using Bee.Toolchain.GNU;
using Bee.Toolchain.VisualStudio;
using Bee.Tools;
using Bee.VisualStudioSolution;
using Newtonsoft.Json.Linq;
using NiceIO;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
using static Bee.NativeProgramSupport.NativeProgramConfiguration;

class Build
{
    private static readonly NPath SourceFolder = "src";
    private static readonly NPath UnitTestSourceFolder = "unittest";

    class TundraNativeProgram : NativeProgram
    {
        public TundraNativeProgram(string name) : base(name)
        {
            //cpp17 on windows to get std::filesystem
            this.CompilerSettingsForMsvc().Add(compiler => compiler.WithCppLanguageVersion(CppLanguageVersion.Cpp17));
            this.CompilerSettingsForGccLike().Add(compiler => compiler.WithVisibility(Visibility.Default));

            this.CompilerSettingsForClang().Add(c => c.WithWarningPolicies(new[]
            {
                new WarningAndPolicy("all", WarningPolicy.AsError)
            }));
            this.CompilerSettingsForMsvc().Add(c => c.WithWarningPolicies(new[]
            {
                //new msvc complains about: destructor was implicitly defined as deleted because a base class destructor is inaccessible or deleted
                new WarningAndPolicy("4624", WarningPolicy.Silent),
                new WarningAndPolicy("4244", WarningPolicy.Silent),
                new WarningAndPolicy("4267", WarningPolicy.Silent), //<-- even vs2017 headers complain about this one
                new WarningAndPolicy("4018", WarningPolicy.AsError),
                new WarningAndPolicy("4189", WarningPolicy.AsError), //local variable is initialized but not referenced
                new WarningAndPolicy("4505", WarningPolicy.AsError) //unreferenced local function has been removed
            }));

            this.Libraries.Add(c=>IsWindows(c) && c.CodeGen == CodeGen.Debug, new SystemLibrary("DbgHelp.lib"));

            // We can enable this by committing valgrind to the repository or uploading a public stevedore artifact.
            this.Defines.Add("USE_VALGRIND=NO");
            this.Defines.Add(IsWindows, "WIN32_LEAN_AND_MEAN", "NOMINMAX", "WINVER=0x0601", "_WIN32_WINNT=0x0601"); // allow using Windows 7+ APIs
            this.DynamicLinkerSettingsForMsvc().Add(linker => linker.WithSubSystemType(SubSystemType.Console));

            //the toolchain we currently use on linux has a many bugs when combining -g and -flto. turn off -g for linux in master builds for now
            this.CompilerSettingsForGcc().Add(c=>c.CodeGen == CodeGen.Master, c=>c.WithDebugMode(DebugMode.MinimalInformation));
        }
    }


    static NPath GenerateGitFile()
    {
        try
        {
            var result = Shell.Execute("git for-each-ref --count 1 --format \"%(objectname):%(refname:short)\"");
            if (!result.Success)
                return null;

            var matches = Regex.Matches(result.StdOut, @"(\w+?):(.+)");
            if (matches.Count == 0)
                return null;

            var hash = matches[0].Groups[0].Captures[0].Value;
            var branch = matches[0].Groups[1].Captures[0].Value;
            var gitRevFile = Configuration.AbsoluteRootArtifactsPath.Combine($"generated/git_rev.c");
            gitRevFile.WriteAllText($@"
                const char g_GitVersion[] = ""${hash}"";
                const char g_GitBranch[]  = ""${branch}"";
            ");
            return gitRevFile;
        }
        catch (Exception) // e.g. Win32Exception if git is not found
        {
            return null;
        }
    }

    static void RegisterAlias(string name, NativeProgramConfiguration config, NPath file)
    {
        Backend.Current.AddAliasDependency(
            $"{name}::{config.ToolChain.LegacyPlatformIdentifier.ToLower()}::{config.CodeGen.ToString().ToLower()}", file);
        Backend.Current.AddAliasDependency($"{name}::{config.ToolChain.LegacyPlatformIdentifier.ToLower()}", file);
        Backend.Current.AddAliasDependency($"{name}::{config.CodeGen.ToString().ToLower()}", file);
        Backend.Current.AddAliasDependency($"{name}", file);
    }

    static string DirNameForConfig(NativeProgramConfiguration config) => $"{config.Platform.Name}-{config.ToolChain.Architecture.Name}";

    static BuiltNativeProgram SetupSpecificConfiguration(NativeProgram program, NativeProgramConfiguration config,
        NativeProgramFormat format)
    {
        var builtProgram = program.SetupSpecificConfiguration(config, format);
        var deployedProgram = builtProgram.DeployTo($"build/{DirNameForConfig(config)}/{config.CodeGen}".ToLower());
        RegisterAlias($"{program.Name}", config, deployedProgram.Path);
        return deployedProgram;
    }

    static void SetupStevedoreArtifact(NativeProgramConfiguration config, string artifactNamePattern,
        IEnumerable<NPath> files)
    {
        // The platform/arch suffix, e.g. "win-x86" or "linux-x64".
        var suffix = $"{config.Platform.DisplayName.ToLower()}-{config.ToolChain.Architecture.DisplayName}";

        var artifactPath = new NPath("artifacts/for-stevedore/" + artifactNamePattern.Replace("*", suffix));

        var contents = new ZipArchiveContents();
        foreach (var path in files)
            contents.AddFileToArchive(path, path.FileName);
        ZipTool.SetupPack(artifactPath, contents);
    }

    static void Main()
    {
        // tundra library
        var tundraLibraryProgram = new TundraNativeProgram("libtundra");
        tundraLibraryProgram.CompilerSettingsForMsvc().Add(compiler => compiler.WithUnicode(false));
        tundraLibraryProgram.Sources.Add(SourceFolder.Files("*.c*").Where(f => !f.FileName.EndsWith("Main.cpp"))
            .ToArray());
        tundraLibraryProgram.PublicIncludeDirectories.Add(SourceFolder);
        tundraLibraryProgram.Libraries.Add(IsWindows,
            new SystemLibrary("Rstrtmgr.lib"),
            new SystemLibrary("Shlwapi.lib"),
            new SystemLibrary("User32.lib")
        );

        // tundra executable
        var tundraExecutableProgram = new TundraNativeProgram("tundra2");
        tundraExecutableProgram.Libraries.Add(tundraLibraryProgram);
        tundraExecutableProgram.Sources.Add(SourceFolder.Combine("Main.cpp"));
        // tundra executable rev info
        var gitRevFile = GenerateGitFile();
        if (gitRevFile != null)
        {
            tundraExecutableProgram.Sources.Add(gitRevFile);
            tundraExecutableProgram.Defines.Add("HAVE_GIT_INFO");
        }

        // workaround to make sure we don't conflict with tundra executable used by bee
        tundraExecutableProgram.ArtifactsGroup = "t2";

        // tundra unit tests
        var tundraUnitTestProgram = new TundraNativeProgram("tundra2-unittest");
        tundraUnitTestProgram.CompilerSettings()
            .Add(compiler =>
                compiler.WithCppLanguageVersion(CppLanguageVersion
                    .Cpp11)); //<-- not Cpp17 yet due to some compile errors in google library
        tundraUnitTestProgram.Libraries.Add(tundraLibraryProgram);
        tundraUnitTestProgram.Sources.Add(UnitTestSourceFolder.Files());
        tundraUnitTestProgram.IncludeDirectories.Add($"{UnitTestSourceFolder}/googletest/googletest");
        tundraUnitTestProgram.IncludeDirectories.Add($"{UnitTestSourceFolder}/googletest/googletest/include");

        var inspectProram = new TundraNativeProgram("tundra2-inspect")
        {
            Libraries = {tundraLibraryProgram},
            Sources = {SourceFolder.Combine("InspectMain.cpp")}
        };



        // setup build targets
        var toolChains = new ToolChain[]
        {
            ToolChain.Store.Mac().Sdk_11_0().x64("10.12"),
            ToolChain.Store.Mac().Sdk_11_0().ARM64("11.0"),
            ToolChain.Store.Windows().VS2019(new Version(16,4)).Sdk_17134().x64(),
            ToolChain.Store.Linux().Ubuntu_14_4().Gcc_4_8().x64(),
        }.Where(toolChain => toolChain.CanBuild).ToArray();

        var configs = toolChains.SelectMany(toolchain => new[]
        {
            new NativeProgramConfiguration(CodeGen.Master, toolchain, lump: false),
            new NativeProgramConfiguration(CodeGen.Debug, toolchain, lump: false),
        });

        var projectFileBuilders =
            new NativeProgram[] {tundraLibraryProgram, tundraExecutableProgram, tundraUnitTestProgram}.ToDictionary(
                p => p, p => new VisualStudioNativeProjectFileBuilder(p));

        foreach (var config in configs)
        {
            var toolchain = config.ToolChain;
            var setupLib = SetupSpecificConfiguration(tundraLibraryProgram, config, toolchain.StaticLibraryFormat);
            projectFileBuilders[tundraLibraryProgram].AddProjectConfiguration(config, setupLib);

            var tundra = SetupSpecificConfiguration(tundraExecutableProgram, config, toolchain.ExecutableFormat);
            projectFileBuilders[tundraExecutableProgram].AddProjectConfiguration(config, tundra);

            var tundraUnitTestExecutable =
                (Executable) SetupSpecificConfiguration(tundraUnitTestProgram, config, toolchain.ExecutableFormat);
            projectFileBuilders[tundraUnitTestProgram].AddProjectConfiguration(config, tundraUnitTestExecutable);

            SetupSpecificConfiguration(inspectProram, config, toolchain.ExecutableFormat);

            if (Bee.PramBinding.Pram.CanLaunch(toolchain.Platform, toolchain.Architecture))
            {
                var tundraUnitTestResult = Bee.PramBinding.Pram.SetupLaunch(
                    new Bee.PramBinding.Pram.LaunchArguments(toolchain.ExecutableFormat, tundraUnitTestExecutable));
                RegisterAlias($"{tundraUnitTestProgram.Name}-report", config, tundraUnitTestResult.Result);
            }

            if (config.CodeGen == CodeGen.Master)
            {
                // Create a zip artifact (rather than 7z), as Bee needs Tundra early
                // (before 7za is downloaded). The artifact should be minimal: Just
                // the license file and main binary, no tests, docs, PDBs, Lua, etc.
                SetupStevedoreArtifact(config, "tundra-*.zip", new[] {"COPYING", tundra.Path});

                // On platforms with separate debug files (currently just Windows),
                // make a separate artifact for these, which devs can fetch as needed.
                if (tundra.Paths.Length > 1)
                {
                    SetupStevedoreArtifact(config, "tundra-*-debug.7z",
                        new NPath[] {"COPYING"}.Concat(tundra.Paths.Skip(1)));
                }
            }

            var configFile = new NPath("bee_config.json").MakeAbsolute();
            if (config.CodeGen == CodeGen.Debug && config.Platform == Platform.HostPlatform && configFile.FileExists())
            {
                JObject configObject = JObject.Parse(configFile.ReadAllText());
                var unityCheckout = (string)configObject["unityCheckout"];
                if (unityCheckout != null)
                {
                    var deployDir = new NPath(unityCheckout).Combine($"External/tundra/builds/build/{DirNameForConfig(config).ToLower()}/master/");
                    var alias = "debug-deploy";
                    Console.WriteLine($"Setting up {alias} alias to {deployDir}");
                    Backend.Current.AddAliasDependency(alias, tundra.DeployTo(deployDir).Path);
                }
            }
        }

        SetupVisualStudioSolution(projectFileBuilders, configs);
    }

    static void SetupVisualStudioSolution(
        Dictionary<NativeProgram, VisualStudioNativeProjectFileBuilder> projectFileBuilders,
        IEnumerable<NativeProgramConfiguration> configs)
    {
        var sln = new VisualStudioSolution()
        {
            Path = "visualstudio/tundra.gen.sln"
        };

        var nativeProjecFiles = projectFileBuilders.Values
            .Select(pfb => pfb.DeployTo($"visualstudio/{pfb.NativeProgram.Name}.gen.vcxproj")).ToArray();
        foreach (var nativeProjectFile in nativeProjecFiles)
            sln.Projects.Add(nativeProjectFile);

        foreach (var config in configs)
        {
            ProjectConfigurationSelector selector = (incomingConfigs, incomingProjectFile) =>
            {
                var projectConfiguration = incomingConfigs.SingleOrDefault(i => i.Identifier == config.Identifier);
                return new Tuple<IProjectConfiguration, bool>(projectConfiguration, projectConfiguration != null);
            };
            sln.Configurations.Add(new SolutionConfiguration(config.Identifier, selector));
        }

        sln.Setup();
    }
}
