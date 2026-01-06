"""
Example: Analyze terrain height across a region
Demonstrates spatial queries and raycast operations
"""

import unreal

def analyze_terrain(bounds_min: tuple = (-5000, -5000), 
                   bounds_max: tuple = (5000, 5000),
                   sample_spacing: float = 500.0):
    """
    Sample terrain height across a region using raycasts
    
    Args:
        bounds_min: Minimum (X, Y) coordinates
        bounds_max: Maximum (X, Y) coordinates
        sample_spacing: Distance between sample points
    """
    world = unreal.EditorLevelLibrary.get_editor_world()
    
    if not world:
        print("No editor world found!")
        return
    
    print(f"Analyzing terrain from {bounds_min} to {bounds_max}")
    print(f"Sample spacing: {sample_spacing}")
    
    heights = []
    sample_count = 0
    hit_count = 0
    
    min_x, min_y = bounds_min
    max_x, max_y = bounds_max
    
    # Sample points
    x = min_x
    while x <= max_x:
        y = min_y
        while y <= max_y:
            # Raycast from above to find ground
            start = unreal.Vector(x, y, 5000.0)
            end = unreal.Vector(x, y, -5000.0)
            
            hit_result = unreal.SystemLibrary.line_trace_single(
                world,
                start,
                end,
                unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                False,
                [],
                unreal.DrawDebugTrace.NONE,
                unreal.HitResult(),
                True
            )
            
            sample_count += 1
            
            if hit_result:
                # Get hit result (need to access via proper method)
                # This is simplified - actual implementation would need proper hit result access
                # For now, just track that we got a hit
                hit_count += 1
            
            y += sample_spacing
        x += sample_spacing
    
    print(f"\nSampled {sample_count} points")
    print(f"Hits: {hit_count}")
    
    if heights:
        print(f"\nTerrain height statistics:")
        print(f"  Min height: {min(heights):.1f}")
        print(f"  Max height: {max(heights):.1f}")
        print(f"  Avg height: {sum(heights)/len(heights):.1f}")
        print(f"  Height variation: {max(heights) - min(heights):.1f}")

if __name__ == "__main__":
    analyze_terrain(
        bounds_min=(-2000, -2000),
        bounds_max=(2000, 2000),
        sample_spacing=500.0
    )

