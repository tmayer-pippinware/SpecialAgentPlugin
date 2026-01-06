import unreal
import random

# Use the Editor Actor Subsystem (UE 5.5 API)
editor_actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
all_actors = editor_actor_subsystem.get_all_level_actors()

# Find tents and props
tents = []
props_with_meshes = []

print("Scanning level for tents and props...")

for actor in all_actors:
    actor_label = actor.get_actor_label().lower()
    
    if 'tent' in actor_label:
        tents.append(actor)
        loc = actor.get_actor_location()
        print(f"Found tent: {actor.get_actor_label()} at ({loc.x:.1f}, {loc.y:.1f}, {loc.z:.1f})")
    elif isinstance(actor, unreal.StaticMeshActor):
        # Exclude large environment pieces
        if not any(keyword in actor_label for keyword in ['floor', 'plane', 'ground', 'landscape', 'sky', 'terrain']):
            static_mesh_comp = actor.static_mesh_component
            if static_mesh_comp and static_mesh_comp.static_mesh:
                props_with_meshes.append({
                    'actor': actor,
                    'mesh': static_mesh_comp.static_mesh,
                    'scale': actor.get_actor_scale3d()
                })

print(f"\nFound {len(tents)} tent(s) and {len(props_with_meshes)} prop(s) to use")

if len(tents) == 0:
    print("No tents found in the level!")
elif len(props_with_meshes) == 0:
    print("No props found to place around tents!")
else:
    # For each tent, place props around it
    num_props_per_tent = min(8, len(props_with_meshes))  # Up to 8 props per tent
    placement_radius = 400.0  # Units around tent
    new_actors = []
    
    for tent in tents:
        tent_loc = tent.get_actor_location()
        print(f"\nPlacing props around {tent.get_actor_label()}...")
        
        # Select random props to place
        selected_props = random.sample(props_with_meshes, min(num_props_per_tent, len(props_with_meshes)))
        
        for i, prop_data in enumerate(selected_props):
            # Calculate position around tent
            angle = (i / num_props_per_tent) * 360.0 + random.uniform(-20, 20)
            distance = random.uniform(placement_radius * 0.6, placement_radius)
            
            import math
            offset_x = math.cos(math.radians(angle)) * distance
            offset_y = math.sin(math.radians(angle)) * distance
            
            new_location = unreal.Vector(
                tent_loc.x + offset_x,
                tent_loc.y + offset_y,
                tent_loc.z  # Keep same Z height
            )
            
            # Spawn new actor with the same mesh
            new_actor = editor_actor_subsystem.spawn_actor_from_class(
                unreal.StaticMeshActor,
                new_location,
                unreal.Rotator(0, random.uniform(0, 360), 0)
            )
            
            if new_actor:
                # Set the mesh
                new_actor.static_mesh_component.set_static_mesh(prop_data['mesh'])
                
                # Vary the scale slightly
                base_scale = prop_data['scale']
                scale_variation = random.uniform(0.8, 1.2)
                new_actor.set_actor_scale3d(unreal.Vector(
                    base_scale.x * scale_variation,
                    base_scale.y * scale_variation,
                    base_scale.z * scale_variation
                ))
                
                # Set a descriptive label
                new_actor.set_actor_label(f"Prop_{prop_data['actor'].get_actor_label()}_{i}")
                new_actors.append(new_actor)
                print(f"  Placed {new_actor.get_actor_label()} at ({new_location.x:.1f}, {new_location.y:.1f})")
    
    print(f"\nSuccessfully placed {len(new_actors)} props around {len(tents)} tent(s)!")
    unreal.log("Props placed successfully around tents!")

