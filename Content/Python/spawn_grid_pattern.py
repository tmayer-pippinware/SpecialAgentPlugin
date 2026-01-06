"""
Example: Spawn actors in a grid pattern
Demonstrates procedural placement via Python
"""

import unreal
import random

def spawn_grid_pattern(rows: int = 5, cols: int = 5, spacing: float = 500.0):
    """
    Spawn actors in a grid pattern
    
    Args:
        rows: Number of rows
        cols: Number of columns
        spacing: Distance between actors
    """
    editor_level_lib = unreal.EditorLevelLibrary()
    
    # Try to find a static mesh to use
    asset_registry = unreal.AssetRegistryHelpers.get_asset_registry()
    all_meshes = asset_registry.get_assets_by_class("StaticMesh", True)
    
    if not all_meshes:
        print("No static meshes found in project!")
        return
    
    # Use first available mesh
    mesh_asset = all_meshes[0]
    print(f"Using mesh: {mesh_asset.asset_name}")
    
    spawned_count = 0
    center_offset_x = -(cols * spacing) / 2
    center_offset_y = -(rows * spacing) / 2
    
    print(f"\nSpawning {rows}x{cols} grid with {spacing} unit spacing...")
    
    for row in range(rows):
        for col in range(cols):
            # Calculate position
            x = center_offset_x + (col * spacing)
            y = center_offset_y + (row * spacing)
            z = 100.0  # Base height
            
            location = unreal.Vector(x, y, z)
            rotation = unreal.Rotator(0, random.uniform(0, 360), 0)
            
            # Spawn actor
            actor = editor_level_lib.spawn_actor_from_class(
                unreal.StaticMeshActor,
                location,
                rotation
            )
            
            if actor:
                # Set the static mesh
                static_mesh_component = actor.static_mesh_component
                if static_mesh_component:
                    static_mesh_component.set_static_mesh(mesh_asset.get_asset())
                
                # Set label
                actor.set_actor_label(f"GridActor_{row}_{col}")
                
                # Random scale variation
                scale = random.uniform(0.8, 1.2)
                actor.set_actor_scale3d(unreal.Vector(scale, scale, scale))
                
                # Add tags for organization
                actor.tags = ["Procedural", "Grid", "Example"]
                
                spawned_count += 1
                
                if spawned_count % 5 == 0:
                    print(f"  Spawned {spawned_count} actors...")
    
    print(f"\n✓ Successfully spawned {spawned_count} actors in grid pattern")
    print("Tip: Select these actors in outliner by searching for tag 'Grid'")

if __name__ == "__main__":
    # Spawn a 10x10 grid
    spawn_grid_pattern(rows=10, cols=10, spacing=300.0)

