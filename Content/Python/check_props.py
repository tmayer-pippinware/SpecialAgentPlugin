import unreal

editor_actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
all_actors = editor_actor_subsystem.get_all_level_actors()

prop_count = 0
for actor in all_actors:
    if actor.get_actor_label().startswith("Prop_"):
        prop_count += 1
        
print(f"Found {prop_count} newly placed props")
unreal.log(f"Found {prop_count} newly placed props")

