"""EchoVRMusic Speakers: place music speakers on an Echo VR map in Blender and export them.

Import the map with lone_echo_blender (1:1 scale, Y-up -> Z-up on), then in the 3D view sidebar
(N) open the EchoVRMusic tab:
  1. Level: the map's level name, e.g. mpl_combat_dyson (Guess reads it from the scene).
  2. Put the 3D cursor where a speaker goes, press Add Speaker. Move the empties as you like.
  3. Export writes <maps folder>/<level>.txt, in the EchoVRMusic project. Run tools/gen_builtin.py
     and setup/build_setup.bat to build those speakers into EchoVRMusic.dll.
Import loads an existing file back as speakers, to edit it.

Coordinates: lone_echo_blender imports game (x, y, z) as Blender (x, -z, y), so a speaker at
Blender (bx, by, bz) is written as game (bx, bz, -by).
"""

bl_info = {
    "name": "EchoVRMusic Speakers",
    "author": "EchoVRMusic",
    "version": (1, 0, 0),
    "blender": (4, 1, 0),
    "location": "3D View > Sidebar > EchoVRMusic",
    "description": "Place music speakers on Echo VR maps for the EchoVRMusic plugin",
    "category": "Import-Export",
}

import os
import re

import bpy

COLLECTION = "EchoVRMusic Speakers"
DEFAULT_MAPS = r"J:\EchoVR-Tools-Launcher\EchoVRMusic\maps"   # then run tools/gen_builtin.py and rebuild


# ---- CSymbol64 (the engine's level name hash), as in lone_echo_blender/scripts/le_symbol_names.py

def _seeds():
    mask = 0x95AC9329AC4BC9B5
    seeds = []
    for i in range(256):
        v = 0x2B5926535897936A if (i & 0x80) else 0
        if i & 0x40:
            v ^= mask
        shift = 0x20
        while shift:
            v = (2 * v) & 0xFFFFFFFFFFFFFFFF
            if i & shift:
                v ^= mask
            shift >>= 1
        seeds.append((2 * v) & 0xFFFFFFFFFFFFFFFF)
    return seeds


SEEDS = _seeds()


def symbol64(text):
    r = 0xFFFFFFFFFFFFFFFF
    for b in text.encode("utf-8", "ignore"):
        if 0x41 <= b <= 0x5A:
            b += 0x20
        r = ((r << 8) & 0xFFFFFFFFFFFFFFFF) ^ SEEDS[(r >> 56) & 0xFF] ^ b
    return r


# ---- helpers

def to_game(v):
    return (v.x, v.z, -v.y)


def to_blender(x, y, z):
    return (x, -z, y)


def speaker_collection(scene, create=True):
    coll = bpy.data.collections.get(COLLECTION)
    if coll is None and create:
        coll = bpy.data.collections.new(COLLECTION)
    if coll is not None and coll.name not in scene.collection.children:
        scene.collection.children.link(coll)
    return coll


def speakers(scene):
    coll = bpy.data.collections.get(COLLECTION)
    objs = list(coll.all_objects) if coll else []
    objs += [o for o in scene.objects if o.get("evm_speaker") and o not in objs]
    return sorted(objs, key=lambda o: o.name)


def new_speaker(context, location):
    obj = bpy.data.objects.new("Speaker", None)
    obj.empty_display_type = "SPHERE"
    obj.empty_display_size = 1.0
    obj.location = location
    obj["evm_speaker"] = True
    speaker_collection(context.scene).objects.link(obj)
    return obj


def maps_dir(context):
    prefs = context.preferences.addons[__name__].preferences
    return bpy.path.abspath(prefs.maps_dir or DEFAULT_MAPS)


def file_for(context):
    return os.path.join(maps_dir(context), context.scene.evm_level.strip() + ".txt")


# ---- operators

class EVM_OT_guess_level(bpy.types.Operator):
    bl_idname = "evm.guess_level"
    bl_label = "Guess"
    bl_description = "Find the level name (mpl_...) in this file's name, collections or objects"

    def execute(self, context):
        names = [bpy.path.basename(bpy.data.filepath)]
        names += [c.name for c in bpy.data.collections] + [o.name for o in context.scene.objects]
        names += [str(o.get("evr_level", "")) for o in context.scene.objects]
        for n in names:
            m = re.search(r"\b(mpl_[a-z0-9_]+)", n.lower())
            if m:
                context.scene.evm_level = m.group(1)
                return {"FINISHED"}
        self.report({"WARNING"}, "No mpl_... name found, type the level name")
        return {"CANCELLED"}


class EVM_OT_add_speaker(bpy.types.Operator):
    bl_idname = "evm.add_speaker"
    bl_label = "Add Speaker"
    bl_description = "Add a music speaker at the 3D cursor"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        obj = new_speaker(context, context.scene.cursor.location.copy())
        for o in context.selected_objects:
            o.select_set(False)
        obj.select_set(True)
        context.view_layer.objects.active = obj
        return {"FINISHED"}


class EVM_OT_export(bpy.types.Operator):
    bl_idname = "evm.export"
    bl_label = "Export"
    bl_description = "Write the speakers to <maps folder>/<level>.txt for EchoVRMusic"

    def execute(self, context):
        level = context.scene.evm_level.strip()
        if not level:
            self.report({"ERROR"}, "Set the level name first")
            return {"CANCELLED"}
        objs = speakers(context.scene)
        if not objs:
            self.report({"ERROR"}, "No speakers: use Add Speaker")
            return {"CANCELLED"}
        path = file_for(context)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        lines = [
            "# EchoVRMusic speakers for %s, made in Blender (%s)" % (level, bpy.path.basename(bpy.data.filepath)),
            "# Echo coordinates in metres: x, y (up), z",
            "Level = %s" % level,
            "LevelId = 0x%016x" % symbol64(level),
        ]
        for o in objs:
            x, y, z = to_game(o.matrix_world.translation)
            lines.append("Speaker = %.3f %.3f %.3f    # %s" % (x, y, z, o.name))
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        self.report({"INFO"}, "Wrote %d speaker(s) to %s" % (len(objs), path))
        return {"FINISHED"}


class EVM_OT_import(bpy.types.Operator):
    bl_idname = "evm.import"
    bl_label = "Import"
    bl_description = "Load <maps folder>/<level>.txt as speakers (replaces the current ones)"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        path = file_for(context)
        if not os.path.isfile(path):
            self.report({"ERROR"}, "No file %s" % path)
            return {"CANCELLED"}
        for o in speakers(context.scene):
            bpy.data.objects.remove(o, do_unlink=True)
        n = 0
        for line in open(path, encoding="utf-8"):
            line = line.split("#", 1)[0]
            if "=" not in line:
                continue
            k, v = (s.strip() for s in line.split("=", 1))
            if k.lower() != "speaker":
                continue
            try:
                x, y, z = (float(t) for t in v.replace(",", " ").split()[:3])
            except ValueError:
                continue
            new_speaker(context, to_blender(x, y, z))
            n += 1
        self.report({"INFO"}, "Loaded %d speaker(s)" % n)
        return {"FINISHED"}


class EVM_OT_open_folder(bpy.types.Operator):
    bl_idname = "evm.open_folder"
    bl_label = "Open Folder"
    bl_description = "Open the maps folder in Explorer"

    def execute(self, context):
        d = maps_dir(context)
        os.makedirs(d, exist_ok=True)
        os.startfile(d)
        return {"FINISHED"}


# ---- UI

class EVM_PT_panel(bpy.types.Panel):
    bl_label = "EchoVRMusic Speakers"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "EchoVRMusic"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        row = layout.row(align=True)
        row.prop(scene, "evm_level", text="Level")
        row.operator("evm.guess_level", text="", icon="VIEWZOOM")
        level = scene.evm_level.strip()
        if level:
            layout.label(text="LevelId 0x%016x" % symbol64(level))
        layout.operator("evm.add_speaker", icon="SPEAKER")
        layout.label(text="%d speaker(s)" % len(speakers(scene)))
        row = layout.row(align=True)
        row.operator("evm.export", icon="EXPORT")
        row.operator("evm.import", icon="IMPORT")
        layout.operator("evm.open_folder", icon="FILE_FOLDER")


class EVM_Preferences(bpy.types.AddonPreferences):
    bl_idname = __name__
    maps_dir: bpy.props.StringProperty(
        name="Maps folder",
        description="The EchoVRMusic project's maps folder (built into the DLL by tools/gen_builtin.py)",
        subtype="DIR_PATH",
        default=DEFAULT_MAPS,
    )

    def draw(self, context):
        self.layout.prop(self, "maps_dir")


CLASSES = (EVM_OT_guess_level, EVM_OT_add_speaker, EVM_OT_export, EVM_OT_import, EVM_OT_open_folder,
           EVM_PT_panel, EVM_Preferences)


def register():
    for c in CLASSES:
        bpy.utils.register_class(c)
    bpy.types.Scene.evm_level = bpy.props.StringProperty(
        name="Level", description="The map's level name, e.g. mpl_combat_dyson")


def unregister():
    del bpy.types.Scene.evm_level
    for c in reversed(CLASSES):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
