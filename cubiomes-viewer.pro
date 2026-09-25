#-------------------------------------------------
#
# Project created by QtCreator 2020-07-11T11:37:33
#
#-------------------------------------------------

QT += core widgets

# uncomment to override the profile compiler
#QMAKE_CC = clang
#QMAKE_CXX = clang++

CHARSET                 = -finput-charset=UTF-8 -fexec-charset=UTF-8
QMAKE_CFLAGS            = $$CHARSET -fwrapv -DSTRUCT_CONFIG_OVERRIDE=1
QMAKE_CXXFLAGS          = $$QMAKE_CFLAGS
QMAKE_CFLAGS_RELEASE    *= -O3 -DNDEBUG
QMAKE_CXXFLAGS_RELEASE  *= -O3 -g3 -DNDEBUG

greaterThan(QT_MAJOR_VERSION, 5) {
    QMAKE_CXXFLAGS += -std=gnu++17
    DEFINES += QT_DISABLE_DEPRECATED_UP_TO=0x050F00
} else {
    # Fixed-seed cave finder uses if constexpr / C++17; Qt5 build uses gnu++17.
    QMAKE_CXXFLAGS += -std=gnu++17
    equals(QMAKE_CXX, g++) {
        QMAKE_CXXFLAGS += -Wno-deprecated-copy
    }
}

win32: {
    CONFIG += static_gnu

    # thank you nullprogram for dealing with the Windows UTF-16 nonsense
    LIBWINSANE          = $$PWD/src/libwinsane
    libwinsane.target   = libwinsane
    libwinsane.output   = $$LIBWINSANE/libwinsane.o
    libwinsane.commands = $(MAKE) -C $$LIBWINSANE -f $$LIBWINSANE/Makefile
    QMAKE_EXTRA_TARGETS += libwinsane
    PRE_TARGETDEPS      += libwinsane
    LIBS                += $$LIBWINSANE/libwinsane.o
} else {
    DEFINES += "LUA_USE_POSIX=1"
}

wasm: {
    DEFINES += "WASM=1"
    #QT_WASM_SOURCE_MAP=1
    QT_WASM_INITIAL_MEMORY = 256MB
    QT_WASM_PTHREAD_POOL_SIZE = 32
    CONFIG(debug, debug|release): {
        #QMAKE_CFLAGS += -O3 -gsource-map
    }
}
#CONFIG += sanitizer
#CONFIG += sanitize_undefined
#CONFIG += sanitize_thread

static_gnu: {
    LIBS += -static -static-libgcc -static-libstdc++
}

CONFIG(debug, debug|release): {
    CUTARGET = debug
} else {
    CUTARGET = release
}

# compile cubiomes (do not pass viewer overlay -I into the library build)
CUPATH              = $$PWD/cubiomes
QMAKE_PRE_LINK      += $(MAKE) -C $$CUPATH -f $$PWD/etc/makefile.cubiomes CC=\"$$QMAKE_CC\" CFLAGS=\"$(CFLAGS) $$CHARSET -fwrapv -DSTRUCT_CONFIG_OVERRIDE=1\" $$CUTARGET
QMAKE_CLEAN         += $$CUPATH/*.o $$CUPATH/libcubiomes.a $$CUPATH/features/*.o
LIBS                += $$CUPATH/libcubiomes.a -lm

# Viewer-side include overlay: avoid pulling huge TerrainNoise into every TU
# without modifying the cubiomes submodule. See src/terrainnoise.h.
# Prepend -I via QMAKE_*FLAGS so it wins over the automatic project-root -I
# (otherwise cubiomes/finders.h resolves to the real submodule header first).
CU_VIEW_INC         = $$OUT_PWD/cubiomes_view_inc
QMAKE_CFLAGS        = -I$$shell_path($$CU_VIEW_INC) $$QMAKE_CFLAGS
QMAKE_CXXFLAGS      = -I$$shell_path($$CU_VIEW_INC) $$QMAKE_CXXFLAGS
INCLUDEPATH         += $$PWD/cubiomes
INCLUDEPATH += \
        $$PWD \
        $$PWD/src \
        $$PWD/src/fixedseed \
        $$PWD/src/fixedseed/monument \
        $$PWD/src/fixedseed/fortress \
        $$PWD/src/fixedseed/river \
        $$PWD/src/fixedseed/cave \
        $$PWD/src/fixedseed/swamphut \
        $$PWD/src/fixedseed/swamphut/lysh \
        $$PWD/src/fixedseed/slime \
        $$PWD/src/fixedseed/slime/opt \
        $$PWD/src/fixedseed/slime/radar

# Avoid object basename clashes (e.g. src/search.cpp vs lysh/search.c).
CONFIG += object_parallel_to_source

# LowYSwampHut lysh core needs -ffp-contract=off for bit-identical FP vs Java.
# Keep it off the global CFLAGS: applying it app-wide slowed dripstone cave vs the JNI DLL.
win32-g++|unix:!macx {
    LYSH_FP_OFF = \
        src/fixedseed/swamphut/lysh/aquifer.c \
        src/fixedseed/swamphut/lysh/biome.c \
        src/fixedseed/swamphut/lysh/carver.c \
        src/fixedseed/swamphut/lysh/caves.c \
        src/fixedseed/swamphut/lysh/climate.c \
        src/fixedseed/swamphut/lysh/column_top.c \
        src/fixedseed/swamphut/lysh/density.c \
        src/fixedseed/swamphut/lysh/eval.c \
        src/fixedseed/swamphut/lysh/interp_noise.c \
        src/fixedseed/swamphut/lysh/noise.c \
        src/fixedseed/swamphut/lysh/phase1.c \
        src/fixedseed/swamphut/lysh/phase2.c \
        src/fixedseed/swamphut/lysh/rng.c \
        src/fixedseed/swamphut/lysh/search.c \
        src/fixedseed/swamphut/lysh/spline.c \
        src/fixedseed/swamphut/lysh/structure.c \
        src/fixedseed/swamphut/lysh/terrain.c
    lysh_fp.name = lysh_fp_contract_off
    lysh_fp.input = LYSH_FP_OFF
    lysh_fp.dependency_type = TYPE_C
    lysh_fp.variable_out = OBJECTS
    # Flat names under OBJECTS_DIR (avoid nested-dir mkdir issues with EXTRA_COMPILERS).
    lysh_fp.output = $${OBJECTS_DIR}${QMAKE_FILE_BASE}_lysh$${first(QMAKE_EXT_OBJ)}
    lysh_fp.commands = $$QMAKE_CC -c $(CFLAGS) -ffp-contract=off $(INCPATH) -o ${QMAKE_FILE_OUT} ${QMAKE_FILE_IN}
    QMAKE_EXTRA_COMPILERS += lysh_fp
} else {
    # Non-gcc: compile lysh via normal SOURCES (no FMA-contract tweak).
    SOURCES += \
        src/fixedseed/swamphut/lysh/aquifer.c \
        src/fixedseed/swamphut/lysh/biome.c \
        src/fixedseed/swamphut/lysh/carver.c \
        src/fixedseed/swamphut/lysh/caves.c \
        src/fixedseed/swamphut/lysh/climate.c \
        src/fixedseed/swamphut/lysh/column_top.c \
        src/fixedseed/swamphut/lysh/density.c \
        src/fixedseed/swamphut/lysh/eval.c \
        src/fixedseed/swamphut/lysh/interp_noise.c \
        src/fixedseed/swamphut/lysh/noise.c \
        src/fixedseed/swamphut/lysh/phase1.c \
        src/fixedseed/swamphut/lysh/phase2.c \
        src/fixedseed/swamphut/lysh/rng.c \
        src/fixedseed/swamphut/lysh/search.c \
        src/fixedseed/swamphut/lysh/spline.c \
        src/fixedseed/swamphut/lysh/structure.c \
        src/fixedseed/swamphut/lysh/terrain.c
}

LUAPATH = $$PWD/lua/src
cu_view_inc.target  = $$CU_VIEW_INC/cubiomes/.stamp
cu_view_inc.depends = $$PWD/cubiomes/finders.h $$PWD/src/terrainnoise.h
win32 {
    cu_view_inc.commands = \
        if not exist $$shell_path($$CU_VIEW_INC\\cubiomes) mkdir $$shell_path($$CU_VIEW_INC\\cubiomes) $$escape_expand(\\n\\t) \
        copy /Y $$shell_path($$PWD/cubiomes/finders.h) $$shell_path($$CU_VIEW_INC/cubiomes/finders.h) $$escape_expand(\\n\\t) \
        copy /Y $$shell_path($$PWD/src/terrainnoise.h) $$shell_path($$CU_VIEW_INC/cubiomes/terrainnoise.h) $$escape_expand(\\n\\t) \
        echo. > $$shell_path($$CU_VIEW_INC/cubiomes/.stamp)
} else {
    cu_view_inc.commands = \
        mkdir -p $$shell_path($$CU_VIEW_INC/cubiomes) $$escape_expand(\\n\\t) \
        cp -f $$shell_path($$PWD/cubiomes/finders.h) $$shell_path($$CU_VIEW_INC/cubiomes/finders.h) $$escape_expand(\\n\\t) \
        cp -f $$shell_path($$PWD/src/terrainnoise.h) $$shell_path($$CU_VIEW_INC/cubiomes/terrainnoise.h) $$escape_expand(\\n\\t) \
        touch $$shell_path($$CU_VIEW_INC/cubiomes/.stamp)
}
QMAKE_EXTRA_TARGETS += cu_view_inc
PRE_TARGETDEPS      += $$CU_VIEW_INC/cubiomes/.stamp
QMAKE_CLEAN         += $$CU_VIEW_INC/cubiomes/finders.h $$CU_VIEW_INC/cubiomes/terrainnoise.h $$CU_VIEW_INC/cubiomes/.stamp

LUAPATH = $$PWD/lua/src

TARGET = cubiomes-viewer

SOURCES += \
        $$LUAPATH/lapi.c \
        $$LUAPATH/lauxlib.c \
        $$LUAPATH/lbaselib.c \
        $$LUAPATH/lcode.c \
        $$LUAPATH/lcorolib.c \
        $$LUAPATH/lctype.c \
        $$LUAPATH/ldblib.c \
        $$LUAPATH/ldebug.c \
        $$LUAPATH/ldo.c \
        $$LUAPATH/ldump.c \
        $$LUAPATH/lfunc.c \
        $$LUAPATH/lgc.c \
        $$LUAPATH/linit.c \
        $$LUAPATH/liolib.c \
        $$LUAPATH/llex.c \
        $$LUAPATH/lmathlib.c \
        $$LUAPATH/lmem.c \
        $$LUAPATH/loadlib.c \
        $$LUAPATH/lobject.c \
        $$LUAPATH/lopcodes.c \
        $$LUAPATH/loslib.c \
        $$LUAPATH/lparser.c \
        $$LUAPATH/lstate.c \
        $$LUAPATH/lstring.c \
        $$LUAPATH/lstrlib.c \
        $$LUAPATH/ltable.c \
        $$LUAPATH/ltablib.c \
        $$LUAPATH/ltm.c \
        $$LUAPATH/lundump.c \
        $$LUAPATH/lutf8lib.c \
        $$LUAPATH/lvm.c \
        $$LUAPATH/lzio.c \
        src/aboutdialog.cpp \
        src/biomecolordialog.cpp \
        src/conditiondialog.cpp \
        src/config.cpp \
        src/configdialog.cpp \
        src/extgendialog.cpp \
        src/exportdialog.cpp \
        src/formconditions.cpp \
        src/formgen48.cpp \
        src/formsearchcontrol.cpp \
        src/gotodialog.cpp \
        src/headless.cpp \
        src/maptoolsdialog.cpp \
        src/message.cpp \
        src/presetdialog.cpp \
        src/layerdialog.cpp \
        src/mapview.cpp \
        src/rangedialog.cpp \
        src/scripts.cpp \
        src/search.cpp \
        src/searchthread.cpp \
        src/tabbiomes.cpp \
        src/tablocations.cpp \
        src/tabstructures.cpp \
        src/tabfixedseed.cpp \
        src/fixedseed/monument/monument_search.cpp \
        src/fixedseed/monument/monument_search_core.c \
        src/fixedseed/fortress/fortress_search.cpp \
        src/fixedseed/river/river_search.cpp \
        src/fixedseed/cave/cave_search.cpp \
        src/fixedseed/cave/BiomeSampler.cpp \
        src/fixedseed/swamphut/swamp_hut_search.cpp \
        src/fixedseed/slime/slime_pipeline.cpp \
        src/fixedseed/slime/opt/geometry.cpp \
        src/fixedseed/slime/opt/java_random.cpp \
        src/fixedseed/slime/opt/optimizer.cpp \
        src/fixedseed/slime/opt/slime_check.cpp \
        src/fixedseed/slime/radar/cpu_search.c \
        src/fixedseed/slime/radar/cpu_simd_stub.c \
        src/mainwindow.cpp \
        src/main.cpp \
        src/util.cpp \
        src/widgets.cpp \
        src/world.cpp

HEADERS += \
        $$CUPATH/finders.h \
        $$CUPATH/generator.h \
        $$CUPATH/layers.h \
        $$CUPATH/biomes.h \
        $$CUPATH/quadbase.h \
        $$CUPATH/util.h \
        $$LUAPATH/lapi.h \
        $$LUAPATH/lauxlib.h \
        $$LUAPATH/lcode.h \
        $$LUAPATH/lctype.h \
        $$LUAPATH/ldebug.h \
        $$LUAPATH/ldo.h \
        $$LUAPATH/lfunc.h \
        $$LUAPATH/lgc.h \
        $$LUAPATH/ljumptab.h \
        $$LUAPATH/llex.h \
        $$LUAPATH/llimits.h \
        $$LUAPATH/lmem.h \
        $$LUAPATH/lobject.h \
        $$LUAPATH/lopcodes.h \
        $$LUAPATH/lopnames.h \
        $$LUAPATH/lparser.h \
        $$LUAPATH/lprefix.h \
        $$LUAPATH/lstate.h \
        $$LUAPATH/lstring.h \
        $$LUAPATH/ltable.h \
        $$LUAPATH/ltm.h \
        $$LUAPATH/lua.h \
        $$LUAPATH/lua.hpp \
        $$LUAPATH/luaconf.h \
        $$LUAPATH/lualib.h \
        $$LUAPATH/lundump.h \
        $$LUAPATH/lvm.h \
        $$LUAPATH/lzio.h \
        src/aboutdialog.h \
        src/biomecolordialog.h \
        src/conditiondialog.h \
        src/config.h \
        src/configdialog.h \
        src/extgendialog.h \
        src/exportdialog.h \
        src/formconditions.h \
        src/formgen48.h \
        src/formsearchcontrol.h \
        src/gotodialog.h \
        src/headless.h \
        src/maptoolsdialog.h \
        src/message.h \
        src/presetdialog.h \
        src/layerdialog.h \
        src/mapview.h \
        src/qzipwriter.h \
        src/rangedialog.h \
        src/scripts.h \
        src/search.h \
        src/searchthread.h \
        src/seedtables.h \
        src/tabbiomes.h \
        src/tablocations.h \
        src/tabstructures.h \
        src/tabfixedseed.h \
        src/mainwindow.h \
        src/util.h \
        src/widgets.h \
        src/world.h

FORMS += \
        src/aboutdialog.ui \
        src/biomecolordialog.ui \
        src/conditiondialog.ui \
        src/configdialog.ui \
        src/extgendialog.ui \
        src/exportdialog.ui \
        src/formconditions.ui \
        src/formgen48.ui \
        src/formsearchcontrol.ui \
        src/gotodialog.ui \
        src/maptoolsdialog.ui \
        src/presetdialog.ui \
        src/layerdialog.ui \
        src/mainwindow.ui \
        src/rangedialog.ui \
        src/tabbiomes.ui \
        src/tablocations.ui \
        src/tabstructures.ui \
        src/tabfixedseed.ui

RESOURCES += \
        rc/icons.qrc \
        rc/style.qrc \
        rc/examples.qrc \
        rc/lang.qrc \
        rc/qh.qrc

# ----- translations (pluralization) -----

TRANSLATIONS += \
        rc/lang/en_US.ts \
        rc/lang/de_DE.ts \
        rc/lang/zh_CN.ts


# enable network features with: qmake CONFIG+=with_network
with_network: {
    QT += network
    DEFINES += "WITH_UPDATER=1"
    SOURCES += src/updater.cpp
    HEADERS += src/updater.h
}

# enable dbus features with: qmake CONFIG+=with_dbus
with_dbus: {
    QT += dbus
    DEFINES += "WITH_DBUS=1"
}
