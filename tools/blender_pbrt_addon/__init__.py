# pbrt-v4 Blender add-on — operators + panel.
#   * Import .pbrt scene  (full material/texture/light importer)
#   * Export .pbrt scene  (scene -> .pbrt)
#   * Render pbrt Scene   (pick a camera, render an existing .pbrt with
#                          pbrt.exe --gpu, show the result in the Image Editor)
bl_info = {
    "name": "pbrt GPU Render",
    "blender": (4, 3, 0),
    "category": "Render",
}

import bpy
import os
import subprocess
import sys
import math
import mathutils

# Add our dir to path so we can import the exporter / importer modules.
import pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))

from exporter import export_pbrt  # noqa: E402

DEFAULT_PBRT = r"F:\project\pbrt-v4\build\Release\pbrt.exe"
DEFAULT_SCENE = r"D:\models\pbrt-v4-scenes\bistro\bistro_cafe_quick.pbrt"


def _camera_items(self, context):
    items = [(c.name, c.name, "") for c in bpy.data.objects
             if c.type == 'CAMERA']
    if not items:
        items = [("NONE", "No cameras", "")]
    return items


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


class PBRT_OT_render_scene(bpy.types.Operator):
    """Render an existing .pbrt scene with pbrt.exe --gpu, using the
    selected Blender camera for the viewpoint. The result is shown in the
    Image Editor."""
    bl_idname = "pbrt.render_scene"
    bl_label = "Render pbrt Scene"

    scene_path: bpy.props.StringProperty(
        name="pbrt scene (.pbrt)",
        description="The .pbrt scene to render (geometry/textures/materials)",
        default=DEFAULT_SCENE,
        subtype='FILE_PATH',
    )
    camera_name: bpy.props.EnumProperty(
        name="Camera",
        description="Blender camera whose viewpoint is used for the render",
        items=_camera_items,
    )

    def _find_camera(self, context):
        cam = bpy.data.objects.get(self.camera_name)
        if cam is None or cam.type != 'CAMERA':
            cam = context.scene.camera
        return cam

    def execute(self, context):
        prefs = None
        if __name__ in context.preferences.addons:
            prefs = context.preferences.addons[__name__].preferences
        pbrt = (prefs.pbrt_path if prefs else "") or DEFAULT_PBRT
        if not os.path.exists(pbrt):
            self.report({'ERROR'}, "pbrt.exe not found: %s" % pbrt)
            return {'CANCELLED'}

        src = bpy.path.abspath(self.scene_path)
        if not os.path.exists(src):
            self.report({'ERROR'}, "pbrt scene not found: %s" % src)
            return {'CANCELLED'}
        scene_dir = os.path.dirname(src)

        cam = self._find_camera(context)
        if cam is None:
            self.report({'ERROR'}, "No camera available for rendering")
            return {'CANCELLED'}

        mw = cam.matrix_world
        pos = mw.translation
        fwd = (mw @ mathutils.Vector((0, 0, -1)) - pos).normalized()
        # pbrt's LookAt uses the up vector literally. The Blender camera's
        # to_track_quat roll would otherwise feed a rolled-up here and rotate
        # the image ~90 deg (EEVEE hides this by forcing world-Y up). Use
        # world +Y projected perpendicular to the view direction instead.
        world_up = mathutils.Vector((0.0, 1.0, 0.0))
        up = (world_up - world_up.dot(fwd) * fwd).normalized()
        tgt = pos + fwd

        def fl(v):
            return "%.6g %.6g %.6g" % (v.x, v.y, v.z)

        fovy = math.degrees(cam.data.angle_y)
        lookat = "LookAt %s %s %s" % (fl(pos), fl(tgt), fl(up))
        camera = 'Camera "perspective" "float fov" [ %.4g ]' % fovy

        # Rewrite the source .pbrt with our camera and a temp output name,
        # keeping the scene's own geometry/textures/materials intact.
        out_name = "pbrt_render_out.exr"
        lines = open(src).read().splitlines()
        out = []
        for ln in lines:
            s = ln.strip()
            if s.startswith("LookAt"):
                out.append(lookat)
            elif s.startswith("Camera"):
                out.append(camera)
            elif s == '"string filename" [ "bistro_cafe_quick.exr" ]':
                # Film output filename only; leave the sky LightSource alone.
                out.append('    "string filename" [ "%s" ]' % out_name)
            else:
                out.append(ln)
        tmp = os.path.join(scene_dir, "pbrt_blender_render.pbrt")
        open(tmp, "w").write("\n".join(out))

        self.report({'INFO'}, "Rendering %s with camera %s ..." %
                    (os.path.basename(src), cam.name))
        # Run pbrt GPU render (blocking; bistro quick is ~40s on this GPU).
        r = subprocess.run([pbrt, "--gpu", os.path.basename(tmp)],
                           cwd=scene_dir, capture_output=True, text=True,
                           timeout=600)
        if r.returncode != 0:
            self.report({'ERROR'}, "pbrt failed (rc=%d): %s" %
                        (r.returncode, r.stderr[-500:]))
            return {'CANCELLED'}

        exr = os.path.join(scene_dir, out_name)
        png = os.path.splitext(exr)[0] + ".png"
        rf = subprocess.run(["ffmpeg", "-y", "-i", exr, png],
                            capture_output=True, text=True, timeout=300)
        if rf.returncode != 0 or not os.path.exists(png):
            self.report({'ERROR'}, "exr->png failed: %s" % rf.stderr[-500:])
            return {'CANCELLED'}

        img = bpy.data.images.load(png, check_existing=True)
        shown = False
        for area in context.screen.areas:
            if area.type == 'IMAGE_EDITOR':
                area.spaces.active.image = img
                shown = True
                break
        self.report({'INFO'}, "Rendered %s -> %s%s" %
                    (img.name, png, "" if shown else " (load it in an Image Editor)"))
        return {'FINISHED'}

    def invoke(self, context, event):
        # Pre-fill the camera enum with the current scene camera.
        cam = context.scene.camera
        if cam is not None and cam.type == 'CAMERA':
            self.camera_name = cam.name
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, context):
        self.layout.prop(self, "scene_path")
        self.layout.prop(self, "camera_name")


class PBRT_PT_panel(bpy.types.Panel):
    bl_label = "pbrt GPU"
    bl_idname = "VIEW3D_PT_pbrt"
    bl_space_type = 'VIEW3D'
    bl_region_type = 'UI'
    bl_category = "pbrt"

    def draw(self, context):
        self.layout.operator(PBRT_OT_import.bl_idname)
        self.layout.operator(PBRT_OT_export.bl_idname)
        self.layout.operator(PBRT_OT_render_scene.bl_idname)


classes = (PBRTAddonPreferences, PBRT_OT_export, PBRT_OT_import,
           PBRT_OT_render_scene, PBRT_PT_panel)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
