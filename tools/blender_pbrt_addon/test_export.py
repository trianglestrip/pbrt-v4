import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import bpy
from exporter import export_pbrt

out = r"D:\models\pbrt-v4-scenes\bistro\blender_default.pbrt"
export_pbrt(bpy.context, out)
print("EXPORTED", out)
