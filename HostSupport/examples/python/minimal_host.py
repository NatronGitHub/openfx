#!/usr/bin/env python3
"""A minimal OFX host in Python, on the HostSupport bindings (the ofxh module).

It renders one frame of one plug-in, in the filter context, on one input image. Use it as a
template: subclass ofxh.Host, ofxh.ImageEffectInstance and ofxh.ClipInstance, and override the
callbacks your host needs. The others keep hostDemo's defaults (a 768x576 project with the PAL
pixel aspect ratio 768/720, 25 frames at 25 fps, one view), which a real host overrides with its
project's: here, the project is the input image, with square pixels.

    PYTHONPATH=<build>/HostSupport/python python minimal_host.py <plugin dir> <plugin id> in.npy out.npy

Images are NumPy arrays (height, width, components), bottom-up (row 0 is the bottom row, as in
OFX images), with 4 (RGBA), 3 (RGB) or 1 (Alpha) components, of dtype uint8, uint16, float16 or
float32 (the clip depth). The input is at pixels (0, 0); the output covers the render window.
"""

import sys

import numpy as np

import ofxh

DEPTHS = {"uint8": "byte", "uint16": "short", "float16": "half", "float32": "float"}
COMPONENTS = {4: "rgba", 3: "rgb", 1: "alpha"}
OK = (ofxh.Stat.OK, ofxh.Stat.ReplyDefault)  # an action that the plug-in doesn't trap replies default


class Host(ofxh.Host):
    """The host (one per process, see ofxh.init()): it shows the plug-ins' messages."""

    def message(self, type, id, text):  # type: error, warning, message, log, question...
        print(f"{type}: {text}", file=sys.stderr)
        # (return a Stat to answer a question, e.g. ofxh.Stat.ReplyYes; None is Stat.OK)


class Clip(ofxh.ClipInstance):
    """A clip. An input gives its image to the plug-in. The output keeps the default: a new
    image over the render window, filled with a sentinel (NaN or 0xCD) to show unwritten pixels."""

    def __init__(self, image=None):
        super().__init__()  # required: it makes the C++ part of the object
        self.image = image  # for an input: the pixels, or None (disconnected)
        if image is not None:  # the depth and components of the input before the clip preferences
            self.unmapped_bit_depth = DEPTHS[image.dtype.name]
            self.unmapped_components = COMPONENTS[image.shape[2]]

    @property
    def connected(self):
        return self.is_output or self.image is not None

    def region_of_definition(self, time, view):
        """The RoD, in canonical coordinates (x is in pixels times the pixel aspect ratio)."""
        if self.is_output or self.image is None:
            return super().region_of_definition(time, view)  # output: the plug-in's RoD
        h, w = self.image.shape[:2]
        return ofxh.pixel_to_canonical((0, 0, w, h), (1., 1.), self.aspect_ratio)

    def get_image(self, time, view, bounds, plane="Colour"):
        """The image for the plug-in's fetch. bounds (canonical, or None) is what it needs: this
        host always gives the whole input. plane is "Colour" (keep the keyword: later parts of
        ofxh pass other planes)."""
        if self.is_output or self.image is None:
            return super().get_image(time, view, bounds, plane)
        h, w = self.image.shape[:2]
        return ofxh.ImageData(self.image, (0, 0, w, h))


class Instance(ofxh.ImageEffectInstance):
    """The effect instance: the project, and the clips. The parameters keep the default of
    new_param(): ofxh's C++ parameters, which store their values and keys (return an ofxh.Param
    subclass instead to hold one in Python)."""

    def __init__(self, inputs):
        super().__init__()
        self.inputs = inputs  # clip name -> image
        source = inputs.get("Source")
        if source is not None:  # the project is the input image, with square pixels
            h, w = source.shape[:2]
            self.project_size = self.project_extent = (w, h)
            self.project_pixel_aspect_ratio = 1.

    def new_clip_instance(self, desc, index):
        return Clip(self.inputs.get(desc.name))


def _check(what, stat):
    if stat not in OK:
        raise RuntimeError(f"{what} failed: {stat!r}")


def render(plugin_dirs, plugin_id, inputs, time=0., use_env_path=True):
    """Render one frame of the plug-in on inputs ({clip name: image}); return the output image
    (an ofxh.ImageData: .pixels, .bounds)."""
    ofxh.init(Host(), plugin_dirs, use_env_path=use_env_path)  # load the plug-ins
    try:
        plugin = ofxh.get_plugin(plugin_id)
        if plugin is None:
            raise LookupError(f"plug-in {plugin_id} not found in {plugin_dirs}")
        inst = ofxh.create_instance(plugin, "filter", Instance(inputs))  # the clips and params
        if inst is None:
            raise RuntimeError(f"can't create an instance of {plugin_id}")
        with inst:  # close(): kOfxActionDestroyInstance, then the instance is deleted
            # (a host that loads a project sets the parameters here, with inst.set_param())
            _check("createInstance", inst.create_instance_action())
            _check("getClipPreferences", inst.get_clip_preferences_action())
            for clip in inst.clips.values():
                # (a real host converts the input when the plug-in remaps its depth or components)
                if clip.image is not None and clip.image.dtype.name != clip.dtype:
                    raise ValueError(f"clip {clip.name}: the plug-in wants {clip.pixel_depth} images")
            stat, rod = inst.get_region_of_definition_action(time)
            _check("getRegionOfDefinition", stat)
            # the render window: the RoD in pixels (at the output clip's PAR), clipped to the project
            r = ofxh.canonical_to_pixel(rod, (1., 1.), inst.clip("Output").aspect_ratio)
            p = inst.project_pixel_rect()
            window = (max(r[0], p[0]), max(r[1], p[1]), min(r[2], p[2]), min(r[3], p[3]))
            _check("beginSequenceRender", inst.begin_sequence_render_action(time, time))
            stat, outputs = inst.render_action(time, window)  # {plane: image}
            _check("render", stat)
            _check("endSequenceRender", inst.end_sequence_render_action(time, time))
            return outputs["Colour"]
    finally:
        ofxh.shutdown()  # unload the plug-ins (also done at exit)


def main(argv):
    if len(argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    plugin_dir, plugin_id, input_path, output_path = argv[1:]
    out = render([plugin_dir], plugin_id, {"Source": np.load(input_path)})
    np.save(output_path, out.pixels)
    print(f"{output_path}: {out.pixels.shape} {out.pixels.dtype}, bounds {out.bounds}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
