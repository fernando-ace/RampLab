using UnrealBuildTool;

public class RampLabViewerTarget : TargetRules
{
    public RampLabViewerTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.AddRange(new[] { "RampLabViewer", "RampLabIntegration" });
    }
}
