/*******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2020 Jean-David Gadina - www.xs-labs.com
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

/*!
 * @file        String.cpp
 * @copyright   (c) 2020, Jean-David Gadina - www.xs-labs.com
 */

#include <XS/String.hpp>
#include <locale>
#include <codecvt>
#include <cctype>

namespace XS
{
    namespace String
    {
        std::string ReplaceAll( const std::string & str, const std::string & search, const std::string & replace )
        {
            if( search == replace || search.length() == 0 )
            {
                return str;
            }
            
            {
                std::string            s( str );
                std::string::size_type pos( 0 );
                
                while( ( pos = s.find( search, pos ) ) != std::string::npos )
                {
                    s.replace( pos, search.length(), replace );
                    
                    pos += replace.length();
                }
                
                return s;
            }
        }
        
        std::optional< std::u16string > ToUTF16( const std::string & str )
        {
            try
            {
                #ifdef __clang__
                #pragma clang diagnostic push
                #pragma clang diagnostic ignored "-Wdeprecated-declarations"
                #endif
                std::wstring_convert< std::codecvt_utf8< char16_t >, char16_t > c;
                #ifdef __clang__
                #pragma clang diagnostic pop
                #endif
                
                return c.from_bytes( str );
            }
            catch( ... )
            {
                return {};
            }
        }
        
        std::optional< std::u32string > ToUTF32( const std::string & str )
        {
            try
            {
                #ifdef __clang__
                #pragma clang diagnostic push
                #pragma clang diagnostic ignored "-Wdeprecated-declarations"
                #endif
                std::wstring_convert< std::codecvt_utf8< char32_t >, char32_t > c;
                #ifdef __clang__
                #pragma clang diagnostic pop
                #endif
                
                return c.from_bytes( str );
            }
            catch( ... )
            {
                return {};
            }
        }
        
        std::optional< std::string > FromUTF16( const std::u16string & str )
        {
            try
            {
                #ifdef __clang__
                #pragma clang diagnostic push
                #pragma clang diagnostic ignored "-Wdeprecated-declarations"
                #endif
                std::wstring_convert< std::codecvt_utf8< char16_t >, char16_t > c;
                #ifdef __clang__
                #pragma clang diagnostic pop
                #endif
                
                return c.to_bytes( str );
            }
            catch( ... )
            {
                return {};
            }
        }
        
        std::optional< std::string > FromUTF32( const std::u32string & str )
        {
            try
            {
                #ifdef __clang__
                #pragma clang diagnostic push
                #pragma clang diagnostic ignored "-Wdeprecated-declarations"
                #endif
                std::wstring_convert< std::codecvt_utf8< char32_t >, char32_t > c;
                #ifdef __clang__
                #pragma clang diagnostic pop
                #endif
                
                return c.to_bytes( str );
            }
            catch( ... )
            {
                return {};
            }
        }
    }
}
