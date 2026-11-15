from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import load
import os.path
import re

required_conan_version = ">=2.0"

# Compile definitions that change the Support/HostSupport class layouts:
# the libraries and their consumers must agree, so they are package options
# and are exported as defines of the package components.
_EXTENSIONS = {
	"natron": "OFX_EXTENSIONS_NATRON",
	"nuke": "OFX_EXTENSIONS_NUKE",
	"vegas": "OFX_EXTENSIONS_VEGAS",
	"tuttle": "OFX_EXTENSIONS_TUTTLE",
	"resolve": "OFX_EXTENSIONS_RESOLVE",
	"opengl_render": "OFX_SUPPORTS_OPENGLRENDER",
	"multithread": "OFX_SUPPORTS_MULTITHREAD",
	"dialog": "OFX_SUPPORTS_DIALOG",
}


class openfx(ConanFile):
	name = "openfx"
	license = "BSD-3-Clause"
	url = "https://github.com/NatronGitHub/openfx"
	description = "OpenFX image processing plug-in standard (NatronGitHub fork: OFX 1.5.1 + Natron, Nuke, Vegas, Tuttle and Resolve extensions)"

	exports_sources = (
		"cmake/*",
		"Examples/*",
		"HostSupport/*",
		"include/*",
		"scripts/*",
		"Support/*",
		"CMakeLists.txt",
		"LICENSE*",
		"README.md",
	)

	def set_version(self):
		# Single source of truth: the project() VERSION in CMakeLists.txt.
		cmakelists = load(self, os.path.join(self.recipe_folder, "CMakeLists.txt"))
		self.version = re.search(r"project\(openfx VERSION ([0-9.]+)", cmakelists).group(1)

	settings = "os", "arch", "compiler", "build_type"
	options = dict(
		{
			"build_examples": [True, False],
			"use_opencl": [True, False],
			"use_system_expat": [True, False],
		},
		**{opt: [True, False] for opt in _EXTENSIONS})
	# Defaults match the Natron plug-ins (openfx-misc, openfx-io, openfx-arena).
	# A host like Natron also sets resolve, multithread and dialog.
	default_options = {
		"build_examples": False,
		"use_opencl": False,
		"use_system_expat": True,  # conan's expat; False builds the bundled HostSupport/expat-2.10.0
		"natron": True,
		"nuke": True,
		"vegas": True,
		"tuttle": True,
		"resolve": False,
		"opengl_render": True,
		"multithread": False,
		"dialog": False,
		"expat/*:shared": False,
	}

	def requirements(self):
		if self.options.use_system_expat:
			# HostSupport parses the plug-in cache with expat; ofxhPluginCache.h includes expat.h
			self.requires("expat/[>=2.7.1 <3]", transitive_headers=True)
		if self.options.build_examples and self.options.use_opencl and self.settings.os != "Macos":
			self.requires("opencl-icd-loader/2023.12.14")
			self.requires("opencl-headers/2023.12.14")

	def layout(self):
		cmake_layout(self)

	def _defines(self):
		return [d for opt, d in _EXTENSIONS.items() if self.options.get_safe(opt)]

	def generate(self):
		deps = CMakeDeps(self)
		deps.generate()

		tc = CMakeToolchain(self)
		tc.cache_variables["BUILD_EXAMPLE_PLUGINS"] = bool(self.options.build_examples)
		tc.cache_variables["OFX_SUPPORTS_OPENCLRENDER"] = bool(self.options.use_opencl)
		tc.cache_variables["OFX_USE_SYSTEM_EXPAT"] = bool(self.options.use_system_expat)
		for opt, define in _EXTENSIONS.items():
			tc.cache_variables[define] = bool(self.options.get_safe(opt))
		if self.settings.os == "Macos":
			# build for the conan arch only (the CMake default is universal)
			tc.cache_variables["CMAKE_OSX_ARCHITECTURES"] = {"armv8": "arm64"}.get(str(self.settings.arch), str(self.settings.arch))
		# example plug-ins go into the package instead of the system OFX folder
		tc.cache_variables["PLUGIN_INSTALLDIR"] = os.path.join(self.package_folder, "bin", "Plugins").replace("\\", "/") if self.package_folder else ""
		tc.generate()

	def build(self):
		cmake = CMake(self)
		cmake.configure()
		cmake.build()

	def package(self):
		cmake = CMake(self)
		cmake.install()
		# install() puts headers in include/ (C API), include/HostSupport, include/Support
		# and include/Support/Plugins, the libraries in lib/, and OpenFX.cmake (add_ofx_plugin)
		# with its Info.plist template in lib/cmake/openfx.

	def package_info(self):
		defines = self._defines()
		if self.settings.os == "Macos":
			defines.append("GL_SILENCE_DEPRECATION")

		# conan generates the CMake config; skip the package's own openfxConfig.cmake
		self.cpp_info.builddirs = []
		self.cpp_info.set_property("cmake_build_modules", [os.path.join("lib", "cmake", "openfx", "OpenFX.cmake")])

		api = self.cpp_info.components["Api"]
		api.includedirs = ["include"]
		api.libdirs = []
		api.bindirs = []
		api.defines = defines

		host = self.cpp_info.components["HostSupport"]
		host.libs = ["OfxHost"]
		host.includedirs = ["include/HostSupport", "include"]
		host.requires = ["Api"]
		host.defines = defines
		if self.options.use_system_expat:
			host.requires.append("expat::expat")
		else:
			host.libs.append("expat")
		if self.settings.os in ("Linux", "FreeBSD"):
			host.system_libs = ["dl"]

		support = self.cpp_info.components["Support"]
		support.libs = ["OfxSupport"]
		support.includedirs = ["include/Support", "include/Support/Plugins"]
		support.requires = ["Api"]
		support.defines = defines
