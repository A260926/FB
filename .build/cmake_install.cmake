# Install script for directory: /Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/.build/libap4.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libap4.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libap4.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libap4.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/bento4" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Codecs/Ap4Ac3Parser.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Codecs/Ap4Ac4Parser.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Codecs/Ap4AdtsParser.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Codecs/Ap4AvcParser.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Codecs/Ap4BitStream.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Codecs/Ap4Eac3Parser.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Codecs/Ap4HevcParser.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Codecs/Ap4Mp4AudioInfo.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Codecs/Ap4NalParser.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap48bdlAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Ac4Utils.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4AinfAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Array.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Atom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4AtomFactory.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4AtomSampleTable.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Av1cAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4AvccAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4BlocAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4ByteStream.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Co64Atom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4ColrAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Command.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4CommandFactory.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4CommonEncryption.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Config.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Constants.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4ContainerAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4CttsAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Dac3Atom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Dac4Atom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4DataBuffer.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Debug.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Dec3Atom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4DecoderConfigDescriptor.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4DecoderSpecificInfoDescriptor.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Descriptor.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4DescriptorFactory.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4DmlpAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4DrefAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4DvccAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4DynamicCast.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4ElstAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4EsDescriptor.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4EsdsAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Expandable.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4File.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4FileByteStream.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4FileCopier.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4FileWriter.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4FragmentSampleTable.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4FrmaAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4FtypAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4GrpiAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4HdlrAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4HintTrackReader.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4HmhdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4HvccAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4IkmsAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Interfaces.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4IodsAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Ipmp.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4IproAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4IsfmAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4IsltAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4IsmaCryp.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4LinearReader.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4List.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Marlin.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4MdhdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4MehdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4MfhdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4MfroAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4MoovAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Movie.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4MovieFragment.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Mpeg2Ts.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4MvhdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4NmhdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4ObjectDescriptor.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4OdafAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4OddaAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4OdheAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4OhdrAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4OmaDcf.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4PdinAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Piff.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Processor.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Protection.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4PsshAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Results.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4RtpAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4RtpHint.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SLConfigDescriptor.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SaioAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SaizAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Sample.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SampleDescription.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SampleEntry.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SampleSource.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SampleTable.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SbgpAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SchmAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SdpAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SegmentBuilder.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SencAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SgpdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SidxAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SmhdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4StcoAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SthdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4String.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4StscAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4StsdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4StssAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4StszAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SttsAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Stz2Atom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4SyntheticSampleTable.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4TencAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4TfdtAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4TfhdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4TfraAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4TimsAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4TkhdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Track.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4TrakAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4TrefTypeAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4TrexAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4TrunAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Types.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4UrlAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Utils.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4UuidAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4Version.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4VmhdAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Core/Ap4VpccAtom.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Crypto/Ap4AesBlockCipher.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Crypto/Ap4Hmac.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Crypto/Ap4KeyWrap.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/Crypto/Ap4StreamCipher.h"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/Source/C++/MetaData/Ap4MetaData.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/.build/generated/bento4Config.cmake"
    "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/.build/generated/bento4ConfigVersion.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/.build/generated/bento4.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4/bento4Targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4/bento4Targets.cmake"
         "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/.build/CMakeFiles/Export/e0dd49b2eb3ca1303803848168356d30/bento4Targets.cmake")
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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4" TYPE FILE FILES "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/.build/CMakeFiles/Export/e0dd49b2eb3ca1303803848168356d30/bento4Targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bento4" TYPE FILE FILES "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/.build/CMakeFiles/Export/e0dd49b2eb3ca1303803848168356d30/bento4Targets-release.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/.build/install_local_manifest.txt"
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
  file(WRITE "/Users/solo/pro/UserFB/workspace/FS9537485EE56AU6638/attempts/20261113T164539570138Z/repo/.build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
