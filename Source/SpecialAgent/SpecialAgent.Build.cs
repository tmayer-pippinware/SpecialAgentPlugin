// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SpecialAgent : ModuleRules
{
	public SpecialAgent(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
		);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
		);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
				"HTTP",
				"Sockets",
				"Networking",
				"HTTPServer",
				"ApplicationCore",
			}
		);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"AssetRegistry",
				"EditorSubsystem",
				"LevelEditor",
				"PythonScriptPlugin",
				"EditorScriptingUtilities",
				"Slate",
				"SlateCore",
				"InputCore",
				"PropertyEditor",
				"Projects",
				"Foliage",
				"Landscape",
				"LandscapeEditor",
				"NavigationSystem",
				"AIModule",
				"ToolMenus",
				"BlueprintGraph",
				"BlueprintEditorLibrary",
				"MaterialEditor",
			}
		);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
		);
	}
}

