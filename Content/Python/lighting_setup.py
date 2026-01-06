"""
Example: Set up basic lighting for a scene
Demonstrates lighting actor creation and configuration
"""

import unreal
import math

def setup_basic_lighting():
    """Set up a basic three-point lighting setup"""
    editor_level_lib = unreal.EditorLevelLibrary()
    
    print("Setting up three-point lighting...")
    
    # 1. Key Light (Main directional light - Sun)
    print("\n1. Creating key light (Sun)...")
    sun = editor_level_lib.spawn_actor_from_class(
        unreal.DirectionalLight,
        unreal.Vector(0, 0, 1000),
        unreal.Rotator(-45, 45, 0)
    )
    if sun:
        sun.set_actor_label("Sun_KeyLight")
        light_component = sun.light_component
        light_component.set_intensity(3.5)
        light_component.set_light_color(unreal.LinearColor(1.0, 0.95, 0.85, 1.0))
        light_component.set_cast_shadows(True)
        print("  ✓ Key light created")
    
    # 2. Sky Light (Fill light)
    print("\n2. Creating sky light...")
    sky_light = editor_level_lib.spawn_actor_from_class(
        unreal.SkyLight,
        unreal.Vector(0, 0, 1000),
        unreal.Rotator(0, 0, 0)
    )
    if sky_light:
        sky_light.set_actor_label("SkyLight_Fill")
        light_component = sky_light.light_component
        light_component.set_intensity(1.0)
        light_component.set_light_color(unreal.LinearColor(0.5, 0.6, 0.7, 1.0))
        print("  ✓ Sky light created")
    
    # 3. Point Lights (Accent lights in a circle)
    print("\n3. Creating accent lights...")
    num_accent_lights = 4
    radius = 2000.0
    
    for i in range(num_accent_lights):
        angle = (2 * math.pi * i) / num_accent_lights
        x = radius * math.cos(angle)
        y = radius * math.sin(angle)
        
        point_light = editor_level_lib.spawn_actor_from_class(
            unreal.PointLight,
            unreal.Vector(x, y, 400),
            unreal.Rotator(0, 0, 0)
        )
        
        if point_light:
            point_light.set_actor_label(f"AccentLight_{i}")
            light_component = point_light.light_component
            light_component.set_intensity(5000.0)
            
            # Different color for each light
            colors = [
                unreal.LinearColor(1.0, 0.8, 0.6, 1.0),  # Warm
                unreal.LinearColor(0.6, 0.8, 1.0, 1.0),  # Cool
                unreal.LinearColor(0.8, 1.0, 0.8, 1.0),  # Green
                unreal.LinearColor(1.0, 0.8, 1.0, 1.0),  # Magenta
            ]
            light_component.set_light_color(colors[i % len(colors)])
            light_component.set_attenuation_radius(1500.0)
            light_component.set_cast_shadows(False)
    
    print(f"  ✓ Created {num_accent_lights} accent lights")
    
    print("\n✓ Lighting setup complete!")
    print("\nLighting configuration:")
    print("  - Directional light as key (sun)")
    print("  - Sky light as ambient fill")
    print(f"  - {num_accent_lights} colored point lights as accents")
    print("\nTip: Adjust light intensities based on your scene needs")

if __name__ == "__main__":
    setup_basic_lighting()

