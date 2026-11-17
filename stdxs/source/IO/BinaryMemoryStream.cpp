/*******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2019 Jean-David Gadina - www.xs-labs.com
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
 * @file        BinaryMemoryStream.cpp
 * @copyright   (c) 2020, Jean-David Gadina - www.xs-labs.com
 */

#include <fstream>
#include <cmath>
#include <vector>
#include <XS/IO/BinaryMemoryStream.hpp>
#include <XS/Casts.hpp>

namespace XS
{
    namespace IO
    {
        class BinaryMemoryStream::IMPL
        {
            public:

                IMPL( const uint8_t * data );
                IMPL( const IMPL & o );
                ~IMPL();
                
                const uint8_t * _data;
                size_t          _pos;
                Endianness      _endianness;
        };
        
        BinaryMemoryStream::BinaryMemoryStream( const uint8_t * data ):
            impl( std::make_unique< IMPL >( data ) )
        {}
        
        BinaryMemoryStream::BinaryMemoryStream( const BinaryMemoryStream & o ):
            impl( std::make_unique< IMPL >( *( o.impl ) ) )
        {}
        
        BinaryMemoryStream::BinaryMemoryStream( BinaryMemoryStream && o ) noexcept:
            impl( std::move( o.impl ) )
        {}
        
        BinaryMemoryStream::~BinaryMemoryStream()
        {}
        
        BinaryMemoryStream & BinaryMemoryStream::operator =( BinaryMemoryStream o )
        {
            swap( *( this ), o );
            
            return *( this );
        }
        
        BinaryMemoryStream::Endianness BinaryMemoryStream::preferredEndianness() const
        {
            return this->impl->_endianness;
        }
        
        void BinaryMemoryStream::setPreferredEndianness( Endianness value )
        {
            this->impl->_endianness = value;
        }
        
        void BinaryMemoryStream::read( uint8_t * buf, size_t size )
        {
            if( size == 0 )
            {
                return;
            }

            memcpy( buf, this->impl->_data + this->impl->_pos, size );
            
            this->impl->_pos += size;
        }
        
        void BinaryMemoryStream::seek( ssize_t offset, SeekDirection dir )
        {
            size_t pos;
            
            if( dir == SeekDirection::Begin )
            {
                if( offset < 0 )
                {
                    throw std::runtime_error( "Invalid seek offset" );
                }
                
                pos = numeric_cast< size_t >( offset );
            }
            else if( dir == SeekDirection::End )
            {
                throw std::runtime_error( "Invalid seek offset" );
            }
            else if( offset < 0 )
            {
                pos = this->impl->_pos - numeric_cast< size_t >( abs( offset ) );
            }
            else
            {
                pos = this->impl->_pos + numeric_cast< size_t >( offset );
            }
            
            this->impl->_pos = pos;
        }
        
        size_t BinaryMemoryStream::tell() const
        {
            return this->impl->_pos;
        }
        
        void swap( BinaryMemoryStream & o1, BinaryMemoryStream & o2 )
        {
            using std::swap;
            
            swap( o1.impl, o2.impl );
        }
        
        BinaryMemoryStream::IMPL::IMPL( const uint8_t * data ):
            _data(       data ),
            _pos(        0 ),
            _endianness( Endianness::Default )
        {}
        
        BinaryMemoryStream::IMPL::IMPL( const IMPL & o ):
            _data(       o._data ),
            _pos(        o._pos ),
            _endianness( o._endianness )
        {}
        
        BinaryMemoryStream::IMPL::~IMPL()
        {}
    }
}
