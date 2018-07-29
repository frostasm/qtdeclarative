CXX_MODULE = qml
TARGET  = qmlsqlstorageplugin
TARGETPATH = QtQuick/SqlStorage
IMPORT_VERSION = 2.$${QT_MINOR_VERSION}

QT = sql qml-private  core-private

SOURCES += plugin.cpp

load(qml_plugin)

OTHER_FILES += sqlstorage.json
