import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.build import can_run
from conan.tools.env import Environment, VirtualRunEnv


class openfxTestConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"

    def requirements(self):
        self.requires(self.tested_reference_str)

    @property
    def _plugin_folder(self):
        # add_ofx_plugin() (cmake/OpenFX.cmake) assembles the bundles in <build>/Plugins
        return os.path.join(self.build_folder, "Plugins")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()

        # Create an environment script that defines the OFX_PLUGIN_PATH
        env1 = Environment()
        env1.define_path("OFX_PLUGIN_PATH", self._plugin_folder)
        env1.vars(self, scope="run").save_script("ofx_plugin_dir")

        vre = VirtualRunEnv(self)
        vre.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        # Build the test_package host (HostSupport) and the invert_plugin bundle (Support).
        cmake.build()

    def layout(self):
        cmake_layout(self)

    def test(self):
        if can_run(self):
            # Run the host in an environment where OFX_PLUGIN_PATH points to the plug-in bundle.
            cmd = os.path.join(self.cpp.build.bindir, "test_package")
            self.run(cmd, env=["conanrun", "ofx_plugin_dir"])
