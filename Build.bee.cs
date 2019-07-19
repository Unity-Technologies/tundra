using System.Linq;
using System.Text.RegularExpressions;
using Bee.Core;
using Bee.NativeProgramSupport.Building;
using Bee.NativeProgramSupport.Building.FluentSyntaxHelpers;
using Bee.Toolchain.GNU;
using Bee.Toolchain.Linux;
using Bee.Toolchain.VisualStudio;
using Bee.Tools;
using NiceIO;
using Unity.BuildSystem.NativeProgramSupport;
using static Unity.BuildSystem.NativeProgramSupport.NativeProgramConfiguration;

class Build
{
    private static readonly NPath SourceFolder = "src";
    private static readonly NPath LuaSourceFolder = "lua/src";
    private static readonly NPath UnitTestSourceFolder = "unittest";

    private static readonly NPath[] LuaSources = new[]
    {
        "lapi.c", "lauxlib.c", "lbaselib.c", "lcode.c",
        "ldblib.c", "ldebug.c", "ldo.c", "ldump.c",
        "lfunc.c", "lgc.c", "linit.c", "liolib.c",
        "llex.c", "lmathlib.c", "lmem.c", "loadlib.c",
        "lobject.c", "lopcodes.c", "loslib.c", "lparser.c",
        "lstate.c", "lstring.c", "lstrlib.c", "ltable.c",
        "ltablib.c", "ltm.c", "lundump.c", "lvm.c",
        "lzio.c"
    }.Select(file => LuaSourceFolder.Combine(file)).ToArray();

    private static readonly NPath[] TundraLuaSources = new[]
    {
        "LuaMain.cpp", "LuaInterface.cpp", "LuaInterpolate.cpp", "LuaJsonWriter.cpp",
        "LuaPath.cpp", "LuaProfiler.cpp"
    }.Select(file => SourceFolder.Combine(file)).ToArray();

    private static readonly NPath[] TundraSources = new[]
    {
        "BinaryWriter.cpp", "BuildQueue.cpp", "Common.cpp", "DagGenerator.cpp",
        "Driver.cpp", "FileInfo.cpp", "Hash.cpp", "HashTable.cpp",
        "IncludeScanner.cpp", "JsonParse.cpp", "JsonWriter.cpp", "MemAllocHeap.cpp",
        "MemAllocLinear.cpp", "MemoryMappedFile.cpp", "PathUtil.cpp", "Profiler.cpp",
        "ScanCache.cpp", "Scanner.cpp", "SignalHandler.cpp", "StatCache.cpp", "SharedResources.cpp",
        "TargetSelect.cpp", "Thread.cpp",
        "ExecUnix.cpp", "ExecWin32.cpp", "DigestCache.cpp", "FileSign.cpp",
        "HashSha1.cpp", "HashFast.cpp", "ConditionVar.cpp", "ReadWriteLock.cpp",
        "Exec.cpp", "NodeResultPrinting.cpp", "OutputValidation.cpp", "re.c", "HumanActivityDetection.cpp"
    }.Select(file => SourceFolder.Combine(file)).ToArray();

    private static readonly NPath[] TundraUnitTestSources = new[]
    {
        "TestHarness.cpp", "Test_BitFuncs.cpp", "Test_Buffer.cpp", "Test_Djb2.cpp", "Test_Hash.cpp",
        "Test_IncludeScanner.cpp", "Test_Json.cpp", "Test_MemAllocLinear.cpp", "Test_Pow2.cpp",
        "Test_TargetSelect.cpp", "test_PathUtil.cpp", "Test_HashTable.cpp", "Test_StripAnsiColors.cpp"
    }.Select(file => UnitTestSourceFolder.Combine(file)).ToArray();


    class TundraNativeProgram : NativeProgram
    {
        public TundraNativeProgram(string name) : base(name)
        {
            this.CompilerSettings().Add(compiler => compiler.WithCppLanguageVersion(CppLanguageVersion.Cpp11));
            this.CompilerSettingsForGccLike().Add(compiler => compiler.WithVisibility(Visibility.Default));

            // We can enable this by committing valgrind to the repository or uploading a public stevedore artifact.
            this.Defines.Add("USE_VALGRIND=NO");
            this.Defines.Add(IsWindows, "WIN32_LEAN_AND_MEAN", "NOMINMAX", "WINVER=0x0600", "_WIN32_WINNT=0x0600");
            this.DynamicLinkerSettingsForMsvc().Add(linker => linker.WithSubSystemType(SubSystemType.Console));
        }
    }

    static NPath GenerateGitFile()
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

    static void RegisterAlias(string name, NativeProgramConfiguration config, NPath file)
    {
        Backend.Current.AddAliasDependency($"{name}::{config.ToolChain.Platform.DisplayName.ToLower()}::{config.CodeGen.ToString().ToLower()}", file);
        Backend.Current.AddAliasDependency($"{name}::{config.ToolChain.Platform.DisplayName.ToLower()}", file);
        Backend.Current.AddAliasDependency($"{name}::{config.CodeGen.ToString().ToLower()}", file);
        Backend.Current.AddAliasDependency($"{name}", file);
    }

    static BuiltNativeProgram SetupSpecificConfiguration(NativeProgram program, NativeProgramConfiguration config, NativeProgramFormat format)
    {
        var builtProgram = program.SetupSpecificConfiguration(config, format);
        var deployedProgram = builtProgram.DeployTo($"build/{config.Platform.Name}-{config.ToolChain.Architecture.Name}/{config.CodeGen}".ToLower());
        RegisterAlias($"{program.Name}", config, deployedProgram.Path);
        return deployedProgram;
    }

    static void Main()
    {
        // lua library
        var luaLibrary = new TundraNativeProgram("lua");
        luaLibrary.CompilerSettingsForMsvc().Add(compiler => compiler.WithUnicode(false));
        luaLibrary.PublicIncludeDirectories.Add(LuaSourceFolder);
        luaLibrary.Sources.Add(LuaSources);

        // tundra library
        var tundraLibraryProgram = new TundraNativeProgram("libtundra");
        tundraLibraryProgram.CompilerSettingsForMsvc().Add(compiler => compiler.WithUnicode(false));
        tundraLibraryProgram.Sources.Add(TundraSources);
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

        // tundra lua executable
        var tundraLuaProgram = new TundraNativeProgram("t2-lua");
        tundraLuaProgram.Libraries.Add(tundraLibraryProgram, luaLibrary);
        tundraLuaProgram.Libraries.Add(IsWindows, new SystemLibrary("Advapi32.lib"));
        tundraLuaProgram.Sources.Add(TundraLuaSources);

        // tundra unit tests
        var tundraUnitTestProgram = new TundraNativeProgram("tundra2-unittest");
        tundraUnitTestProgram.Libraries.Add(tundraLibraryProgram);
        tundraUnitTestProgram.Sources.Add(TundraUnitTestSources);
        tundraUnitTestProgram.IncludeDirectories.Add($"{UnitTestSourceFolder}/googletest/googletest");
        tundraUnitTestProgram.IncludeDirectories.Add($"{UnitTestSourceFolder}/googletest/googletest/include");

        // setup build targets
        foreach (var toolchain in new ToolChain[]
        {
            ToolChain.Store.Mac().Sdk_10_13().x64("10.12"),
            ToolChain.Store.Windows().VS2017().Sdk_17134().x64(),
            ToolChain.Store.Linux().Ubuntu_14_4().Gcc_4_8().x64(),
            new LinuxGccToolchain(WSLGccSdk.Locatorx64.UserDefaultOrDummy),
        })
        {
            foreach (var config in new[]
            {
                new NativeProgramConfiguration(CodeGen.Master, toolchain, lump: false),
                new NativeProgramConfiguration(CodeGen.Debug, toolchain, lump: false),
            })
            {
                if (!toolchain.CanBuild)
                    continue;

                SetupSpecificConfiguration(tundraLibraryProgram, config, toolchain.StaticLibraryFormat);
                SetupSpecificConfiguration(tundraLuaProgram, config, toolchain.ExecutableFormat);
                SetupSpecificConfiguration(tundraExecutableProgram, config, toolchain.ExecutableFormat);

                var tundraUnitTestExecutable = (Executable)SetupSpecificConfiguration(tundraUnitTestProgram, config, toolchain.ExecutableFormat);
                if (Bee.PramBinding.Pram.CanLaunch(toolchain.Platform, toolchain.Architecture))
                {
                    var tundraUnitTestResult = Bee.PramBinding.Pram.SetupLaunch(new Bee.PramBinding.Pram.LaunchArguments(toolchain.ExecutableFormat, tundraUnitTestExecutable));
                    RegisterAlias($"{tundraUnitTestProgram.Name}-report", config, tundraUnitTestResult.Result);
                }
            }
        }
    }
}
