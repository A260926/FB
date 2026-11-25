/*****************************************************************
|
|    AP4 - Edited Movie to MPEG2-TS Writer
|
|    Copyright 2002-2025 Axiomatic Systems, LLC
|
|    This file is part of Bento4/AP4.
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2, or (at your option)
|    any later version.
|
 ****************************************************************/

#ifndef _AP4_EDITED_TS_H_
#define _AP4_EDITED_TS_H_

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4Types.h"

/*----------------------------------------------------------------------
|   forward declarations
+---------------------------------------------------------------------*/
class AP4_Movie;
class AP4_ByteStream;

/*----------------------------------------------------------------------
|   AP4_EditedTsWriter
+---------------------------------------------------------------------*/
class AP4_EditedTsWriter
{
public:
    /**
     * Write the edit-list timeline of an ordinary self-contained MP4 movie
     * as one single-program MPEG-2 Transport Stream.
     */
    static AP4_Result Write(AP4_Movie&       movie,
                            AP4_ByteStream&  output,
                            AP4_UI64         origin90k,
                            AP4_UI32         pcrInterval90k);

private:
    AP4_EditedTsWriter();
};

#endif // _AP4_EDITED_TS_H_
