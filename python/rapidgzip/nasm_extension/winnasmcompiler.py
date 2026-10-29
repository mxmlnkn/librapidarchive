"""
Distutils doesn't support nasm, so this is a custom compiler for NASM
"""

from distutils.errors import DistutilsExecError, CompileError

try:
    from distutils.msvc9compiler import MSVCCompiler
except ImportError:
    from setuptools._distutils._msvccompiler import MSVCCompiler
import inspect
import os
import shutil


class WinNasmCompiler(MSVCCompiler):
    compiler_type = 'winnasm'
    src_extensions = ['.asm']

    def __init__(self, verbose=0, dry_run=0, force=0):

        # Setuptools 84 removed the dry_run argument from the compiler constructor.
        if "dry_run" in inspect.signature(MSVCCompiler.__init__).parameters:
            MSVCCompiler.__init__(self, verbose, dry_run, force)
        else:
            MSVCCompiler.__init__(self, verbose, force)
            self.dry_run = dry_run

    def find_exe(self, exe):
        find_exe = getattr(super(), "find_exe", None)
        if find_exe is not None:
            return find_exe(exe)
        path = shutil.which(exe)
        if path is None:
            raise DistutilsExecError(f"Could not find {exe}")
        return path

    def initialize(self, plat_name=None):
        self.cc = self.find_exe("nasm.exe")
        self.linker = self.find_exe("link.exe")
        self.lib = self.find_exe("lib.exe")
        self.rc = self.find_exe("rc.exe")  # resource compiler
        self.mc = self.find_exe("mc.exe")  # message compiler

        self.compile_options = ["-f win64", "-DWINDOWS", "-DNOPIE"]
        self.compile_options_debug = ["-f win64", "-g", "-DWINDOWS", "-DNOPIE"]
        self.ldflags_shared = ['/DLL', '/nologo', '/INCREMENTAL:NO', '/NOENTRY']
        self.ldflags_shared_debug = ['/DLL', '/nologo', '/INCREMENTAL:NO', '/DEBUG', '/NOENTRY']
        self.ldflags_static = ['/nologo']
        self.initialized = True

    def link(
        self,
        target_desc,
        objects,
        output_filename,
        output_dir=None,
        libraries=None,
        library_dirs=None,
        runtime_library_dirs=None,
        export_symbols=None,
        debug=0,
        extra_preargs=None,
        extra_postargs=None,
        build_temp=None,
        target_lang=None,
    ):

        return super().link(
            target_desc,
            objects,
            output_filename,
            output_dir,
            libraries,
            library_dirs,
            runtime_library_dirs,
            export_symbols,
            debug,
            extra_preargs,
            extra_postargs,
            build_temp,
            target_lang,
        )

    def compile(
        self,
        sources,
        output_dir=None,
        macros=None,
        include_dirs=None,
        debug=0,
        extra_preargs=None,
        extra_postargs=None,
        depends=None,
    ):

        if not self.initialized:
            self.initialize()
        compile_info = self._setup_compile(output_dir, macros, include_dirs, sources, depends, extra_postargs)
        macros, objects, extra_postargs, pp_opts, build = compile_info

        compile_opts = extra_preargs or []

        if debug:
            compile_opts.extend(self.compile_options_debug)
        else:
            compile_opts.extend(self.compile_options)

        for obj in objects:
            try:
                src, ext = build[obj]
            except KeyError:
                continue
            if debug:
                # pass the full pathname to MSVC in debug mode,
                # this allows the debugger to find the source file
                # without asking the user to browse for it
                src = os.path.abspath(src)

            input_opt = src
            output_opt = "-o" + obj
            try:
                self.spawn([self.cc] + compile_opts + pp_opts + [input_opt, output_opt] + extra_postargs)
            except DistutilsExecError as msg:
                raise CompileError(msg)

        return objects
