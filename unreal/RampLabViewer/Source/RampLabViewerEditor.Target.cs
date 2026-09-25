using UnrealBuildTool;

public class RampLabViewerEditorTarget : TargetRules
{
    public RampLabViewerEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.AddRange(new[] { "RampLabViewer", "RampLabIntegration" });
    }
}
