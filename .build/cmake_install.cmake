# Install script for directory: /Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/.build/libap4.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libap4.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libap4.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libap4.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/bento4" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Codecs/Ap4Ac3Parser.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Codecs/Ap4Ac4Parser.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Codecs/Ap4AdtsParser.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Codecs/Ap4AvcParser.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Codecs/Ap4BitStream.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Codecs/Ap4Eac3Parser.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Codecs/Ap4HevcParser.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Codecs/Ap4Mp4AudioInfo.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Codecs/Ap4NalParser.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap48bdlAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Ac4Utils.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4AinfAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Array.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Atom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4AtomFactory.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4AtomSampleTable.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Av1cAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4AvccAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4BlocAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4ByteStream.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Co64Atom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4ColrAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Command.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4CommandFactory.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4CommonEncryption.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Config.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Constants.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4ContainerAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4CttsAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Dac3Atom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Dac4Atom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4DataBuffer.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Debug.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Dec3Atom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4DecoderConfigDescriptor.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4DecoderSpecificInfoDescriptor.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Descriptor.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4DescriptorFactory.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4DmlpAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4DrefAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4DvccAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4DynamicCast.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4ElstAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4EsDescriptor.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4EsdsAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Expandable.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4File.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4FileByteStream.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4FileCopier.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4FileWriter.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4FragmentSampleTable.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4FrmaAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4FtypAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4GrpiAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4HdlrAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4HintTrackReader.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4HmhdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4HvccAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4IkmsAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Interfaces.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4IodsAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Ipmp.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4IproAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4IsfmAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4IsltAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4IsmaCryp.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4LinearReader.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4List.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Marlin.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4MdhdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4MehdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4MfhdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4MfroAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4MoovAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Movie.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4MovieFragment.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Mpeg2Ts.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4MvhdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4NmhdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4ObjectDescriptor.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4OdafAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4OddaAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4OdheAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4OhdrAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4OmaDcf.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4PdinAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Piff.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Processor.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Protection.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4PsshAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Results.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4RtpAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4RtpHint.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SLConfigDescriptor.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SaioAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SaizAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Sample.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SampleDescription.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SampleEntry.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SampleSource.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SampleTable.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SbgpAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SchmAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SdpAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SegmentBuilder.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SencAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SgpdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SidxAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SmhdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4StcoAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SthdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4String.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4StscAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4StsdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4StssAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4StszAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SttsAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Stz2Atom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4SyntheticSampleTable.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4TencAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4TfdtAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4TfhdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4TfraAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4TimsAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4TkhdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Track.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4TrakAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4TrefTypeAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4TrexAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4TrunAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Types.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4UrlAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Utils.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4UuidAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4Version.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4VmhdAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Core/Ap4VpccAtom.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Crypto/Ap4AesBlockCipher.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Crypto/Ap4Hmac.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Crypto/Ap4KeyWrap.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/Crypto/Ap4StreamCipher.h"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/Source/C++/MetaData/Ap4MetaData.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/.build/generated/bento4Config.cmake"
    "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/.build/generated/bento4ConfigVersion.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/.build/generated/bento4.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4/bento4Targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4/bento4Targets.cmake"
         "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/.build/CMakeFiles/Export/e0dd49b2eb3ca1303803848168356d30/bento4Targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4/bento4Targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4/bento4Targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/.build/CMakeFiles/Export/e0dd49b2eb3ca1303803848168356d30/bento4Targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4" TYPE FILE FILES "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/.build/CMakeFiles/Export/e0dd49b2eb3ca1303803848168356d30/bento4Targets-release.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/.build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FSEB84EE1A37D9U16843/attempts/20261127T180818111888Z/repo/.build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
