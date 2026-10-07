using UnrealBuildTool;

public class Loom : ModuleRules
{
	public Loom(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Engine only: Loom never depends on a game or on Tessera.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Json" });
	}
}
