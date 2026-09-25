using UnrealBuildTool;

public class RampLabViewer : ModuleRules
{
    public RampLabViewer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RampLabIntegration" });
    }
}
