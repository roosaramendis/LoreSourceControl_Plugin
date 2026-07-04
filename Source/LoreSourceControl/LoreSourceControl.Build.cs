using UnrealBuildTool;

public class LoreSourceControl : ModuleRules
{
    public LoreSourceControl(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "SourceControl",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "Slate",
            "SlateCore",
            "UnrealEd",
            "ToolMenus",
            "InputCore",
            "SourceControlWindows",
        });
    }
}