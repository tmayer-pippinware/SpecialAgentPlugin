"""
Example: Find and list actors by type
Demonstrates world introspection via Python
"""

import unreal

def find_actors_by_type(actor_class_name: str = None):
    """
    Find all actors of a specific type in the current level
    
    Args:
        actor_class_name: Name of the actor class (e.g., "StaticMeshActor", "PointLight")
    """
    editor_level_lib = unreal.EditorLevelLibrary()
    all_actors = editor_level_lib.get_all_level_actors()
    
    print(f"Total actors in level: {len(all_actors)}")
    
    if actor_class_name:
        # Filter by class
        filtered_actors = [a for a in all_actors if actor_class_name in a.get_class().get_name()]
        print(f"\nActors of type '{actor_class_name}': {len(filtered_actors)}")
        
        for i, actor in enumerate(filtered_actors[:20]):  # Show first 20
            location = actor.get_actor_location()
            print(f"  {i+1}. {actor.get_actor_label()}")
            print(f"     Location: X={location.x:.1f}, Y={location.y:.1f}, Z={location.z:.1f}")
        
        if len(filtered_actors) > 20:
            print(f"  ... and {len(filtered_actors) - 20} more")
    else:
        # Show breakdown by type
        print("\nActor breakdown by type:")
        type_counts = {}
        for actor in all_actors:
            class_name = actor.get_class().get_name()
            type_counts[class_name] = type_counts.get(class_name, 0) + 1
        
        for class_name, count in sorted(type_counts.items(), key=lambda x: x[1], reverse=True):
            print(f"  {class_name}: {count}")

if __name__ == "__main__":
    # List all actors
    find_actors_by_type()
    
    # Example: Find all static mesh actors
    print("\n" + "="*60 + "\n")
    find_actors_by_type("StaticMeshActor")

