# SpecialAgent Example Python Scripts

Example Python scripts that run inside Unreal Engine to demonstrate various capabilities.

## Usage

These scripts can be executed in Unreal Engine in several ways:

### 1. Via MCP Client (Recommended)
```python
import asyncio
from mcp_client import MCPClient

async def run():
    client = MCPClient()
    await client.connect()
    
    # Read and execute a script file
    with open("Content/Python/list_all_assets.py") as f:
        code = f.read()
    
    result = await client.execute_python(code)
    print(result['stdout'])
    
    await client.disconnect()

asyncio.run(run())
```

### 2. Via UE Python Console
In Unreal Engine, open the Python console and run:
```python
import sys
sys.path.append("A:/Exordium/SpecialAgent/Content/Python")
import list_all_assets
list_all_assets.list_all_assets()
```

### 3. Via UE Python Editor Utility
1. Create a Python Editor Utility Widget
2. Add a button that executes these scripts
3. Click the button to run

## Example Scripts

### list_all_assets.py
Lists all assets in the Content Browser with breakdown by type.

**What it demonstrates:**
- Asset Registry usage
- Asset enumeration
- Asset classification

### find_actors_by_type.py
Finds and lists actors in the current level by type.

**What it demonstrates:**
- World introspection
- Actor queries
- Actor property access

### spawn_grid_pattern.py
Spawns actors in a procedural grid pattern.

**What it demonstrates:**
- Actor spawning
- Procedural placement
- Transform manipulation
- Component configuration

### terrain_analysis.py
Analyzes terrain height across a region using raycasts.

**What it demonstrates:**
- Spatial queries
- Raycast operations
- Terrain sampling
- Statistical analysis

### lighting_setup.py
Creates a three-point lighting setup for a scene.

**What it demonstrates:**
- Lighting actor spawning
- Light configuration
- Color and intensity adjustment
- Procedural light placement

## Core Principles

These examples demonstrate the **Python-first** philosophy:

1. **Python = Primary Control** - All operations use Python API directly
2. **No Limitations** - Full access to UE5 API
3. **Composable** - Combine and extend these examples
4. **Inspectable** - All code is readable and modifiable

## Integration with Screenshot Workflow

The typical workflow:

```python
# 1. Execute Python to build something
result = await client.execute_python(code)

# 2. Capture screenshot
screenshot = await client.capture_screenshot()

# 3. Analyze with LLM vision
# [LLM sees: "Trees too sparse, needs more variation"]

# 4. Refine Python based on feedback
refined_code = """
# Increase density and add variation...
"""
result = await client.execute_python(refined_code)

# 5. Capture again to verify
screenshot = await client.capture_screenshot()

# Repeat until perfect!
```

## Tips

- Always print progress and results
- Use descriptive actor labels
- Add tags for organization
- Handle errors gracefully
- Test with small examples first
- Save your work frequently

## Next Steps

- Combine multiple examples
- Create custom workflows
- Build domain-specific tools
- Experiment with different parameters
- Share your creations!

