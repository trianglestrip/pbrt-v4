# pbrt-v4 Blender add-on — operators + panel ("Render (pbrt GPU)" button).
bl_info = {
    "name": "pbrt GPU Render",
    "blender": (4, 3, 0),
    "category": "Render",
}

import bpy
import os
import subprocess
import sys

# Add our dir to path so we can import the exporter module.
import pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))

from exporter import export_pbrt  # noqa: E402

DEFAULT_PBRT = r"F:\project\pbrt-v4\build\Release\pbrt.exe"


class PBRTAddonPreferences(bpy.types.AddonPreferences):
    bl_idname = __name__
    pbrt_path: bpy.props.StringProperty(
        name="pbrt.exe path",
        default=DEFAULT_PBRT,
        subtype='FILE_PATH',
    )

    def draw(self, context):
        self.layout.prop(self, "pbrt_path")


class PBRT_OT_export(bpy.types.Operator):
    bl_idname = "pbrt.export"
    bl_label = "Export .pbrt"
    filepath: bpy.props.StringProperty(subtype='FILE_PATH', default="//scene.pbrt")

    def execute(self, context):
        path = bpy.path.abspath(self.filepath)
        if not path.endswith(".pbrt"):
            path += ".pbrt"
        export_pbrt(context, path)
        self.report({'INFO'}, "Exported %s" % path)
        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}


class PBRT_OT_render(bpy.types.Operator):
    bl_idname = "pbrt.render"
    bl_label = "Render (pbrt GPU)"

    def execute(self, context):
        prefs = context.preferences.addons[__name__].preferences
        pbrt = prefs.pbrt_path or DEFAULT_PBRT
        if not os.path.exists(pbrt):
            self.report({'ERROR'}, "pbrt.exe not found: %s" % pbrt)
            return {'CANCELLED'}

        scene = context.scene
        out_dir = bpy.path.abspath("//") or os.path.dirname(bpy.data.filepath) or os.getcwd()
        pbrt_path = os.path.join(out_dir, "blender_export.pbrt")
        export_pbrt(context, pbrt_path)
        self.report({'INFO'}, "Exported %s, launching pbrt --gpu" % pbrt_path)

        # Run pbrt GPU render (non-blocking so Blender stays responsive).
        scene_dir = os.path.dirname(pbrt_path)
        subprocess.Popen([pbrt, "--gpu", os.path.basename(pbrt_path)], cwd=scene_dir)
        return {'FINISHED'}


class PBRT_OT_import(bpy.types.Operator):
    bl_idname = "pbrt.import_scene"
    bl_label = "Import .pbrt"
    filepath: bpy.props.StringProperty(subtype='FILE_PATH')

    def execute(self, context):
        import pbrt_importer
        path = bpy.path.abspath(self.filepath)
        r = pbrt_importer.import_pbrt(path, 'PBRT')
        self.report({'INFO'}, "Imported %s: %s shapes" %
                    (path, r.get('shapes', '?')))
        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}


class PBRT_PT_panel(bpy.types.Panel):
    bl_label = "pbrt GPU"
    bl_idname = "VIEW3D_PT_pbrt"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "pbrt"

    def draw(self, context):
        self.layout.operator(PBRT_OT_import.bl_idname)
        self.layout.operator(PBRT_OT_export.bl_idname)
        self.layout.operator(PBRT_OT_render.bl_idname)


classes = (PBRTAddonPreferences, PBRT_OT_export, PBRT_OT_render,
           PBRT_OT_import, PBRT_PT_panel)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
