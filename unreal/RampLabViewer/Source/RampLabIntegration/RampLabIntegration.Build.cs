using System.IO;
using UnrealBuildTool;

public class RampLabIntegration : ModuleRules
{
    public RampLabIntegration(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp23;
        bEnableExceptions = true;

        PublicDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "CesiumRuntime", "InputCore", "Slate", "SlateCore",
            "Sockets", "Networking"
        });

        string RepositoryRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", ".."));
        string CoreBuild = Path.Combine(RepositoryRoot, "build-unreal-core-v143");
        PublicIncludePaths.Add(Path.Combine(RepositoryRoot, "include"));

        PublicAdditionalLibraries.Add(Path.Combine(CoreBuild, "Release", "airside_sim.lib"));
        PublicAdditionalLibraries.Add(Path.Combine(CoreBuild, "Release", "airside_autonomy.lib"));
        PublicAdditionalLibraries.Add(Path.Combine(CoreBuild, "Release", "airside_autonomy_scenario.lib"));
        PublicAdditionalLibraries.Add(Path.Combine(CoreBuild, "Release", "airside_scenario.lib"));
        PublicAdditionalLibraries.Add(Path.Combine(CoreBuild, "_deps", "yaml-cpp-build", "Release", "yaml-cpp.lib"));

        RuntimeDependencies.Add(
            "$(TargetOutputDir)/Scenarios/baseline.yaml",
            Path.Combine(RepositoryRoot, "scenarios", "baseline.yaml"));
        RuntimeDependencies.Add(
            "$(TargetOutputDir)/Scenarios/high_capacity.yaml",
            Path.Combine(RepositoryRoot, "scenarios", "high_capacity.yaml"));
        RuntimeDependencies.Add(
            "$(TargetOutputDir)/Scenarios/autonomy_tug.yaml",
            Path.Combine(RepositoryRoot, "scenarios", "autonomy_tug.yaml"));
        RuntimeDependencies.Add(
            "$(TargetOutputDir)/Scenarios/autonomy_sensor_validation.yaml",
            Path.Combine(RepositoryRoot, "scenarios", "autonomy_sensor_validation.yaml"));
        RuntimeDependencies.Add("$(TargetOutputDir)/Scenarios/autonomy_fleet.yaml",Path.Combine(RepositoryRoot,"scenarios","autonomy_fleet.yaml"));
        RuntimeDependencies.Add("$(TargetOutputDir)/Scenarios/autonomy_fleet_fault.yaml",Path.Combine(RepositoryRoot,"scenarios","autonomy_fleet_fault.yaml"));
    }
}
