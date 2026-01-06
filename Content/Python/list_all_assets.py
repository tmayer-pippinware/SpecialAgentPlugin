"""
Example: List all assets in Content Browser
Demonstrates asset discovery via Python
"""

import unreal

def list_all_assets():
    """List all assets in the project"""
    asset_registry = unreal.AssetRegistryHelpers.get_asset_registry()
    
    # Get all assets
    all_assets = asset_registry.get_all_assets()
    
    print(f"Total assets in project: {len(all_assets)}")
    print("\nAsset breakdown by class:")
    
    # Count by class
    class_counts = {}
    for asset_data in all_assets:
        class_name = str(asset_data.asset_class_path.asset_name)
        class_counts[class_name] = class_counts.get(class_name, 0) + 1
    
    # Print sorted by count
    for class_name, count in sorted(class_counts.items(), key=lambda x: x[1], reverse=True):
        print(f"  {class_name}: {count}")
    
    # Show some example assets
    print("\nExample assets (first 20):")
    for i, asset_data in enumerate(all_assets[:20]):
        asset_name = str(asset_data.asset_name)
        asset_path = asset_data.object_path
        print(f"  {i+1}. {asset_name} ({asset_data.asset_class_path.asset_name})")
        print(f"     Path: {asset_path}")

if __name__ == "__main__":
    list_all_assets()

