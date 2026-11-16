// Generated from locked stb SHA256 b04df6aa2fdd476ebcddcafd8e634014e34d29f89f6b648b137f2efa66f3bd47
static Task pause_stbir__vertical_gather_loop( stbir__info const * stbir_info, stbir__per_split_info* split_info, int split_count, stbir__column_slice const * slice )
{
  stbir__sampler const * vertical = &stbir_info->vertical;
  int y, start_output_y, end_output_y;

  STBIR_ASSERT( vertical->is_gather );

  start_output_y = split_info->start_output_y;
  end_output_y = split_info[split_count-1].end_output_y;

  // initialize the ring buffer for gathering: start it at the lowest scanline the first rows need
  {
    int look = vertical->filter_pixel_margin * 3, unused;
    if ( look > ( end_output_y - start_output_y - 1 ) )
      look = end_output_y - start_output_y - 1;
    split_info->ring_buffer_begin_index = 0;
    split_info->ring_buffer_first_scanline = stbir__lowest_scanline_needed( vertical, start_output_y, look, &unused );
    split_info->ring_buffer_last_scanline = split_info->ring_buffer_first_scanline - 1; // means "empty"
  }

  for (y = start_output_y; y < end_output_y; y++)
  {
    int in_first_scanline, in_last_scanline;
    float const * vertical_coefficients = stbir__output_coefficients( vertical, y );

    stbir__output_range( vertical, y, &in_first_scanline, &in_last_scanline );

    // rows with a zero weight at either end aren't worth decoding (the pack's shifted sets when the vertical shares the
    //   horizontal's tables, kernels that are exactly zero at their support's edge)
    while ( ( in_first_scanline < in_last_scanline ) && ( vertical_coefficients[0] == 0.0f ) )
    {
      ++in_first_scanline;
      ++vertical_coefficients;
    }
    while ( ( in_last_scanline > in_first_scanline ) && ( vertical_coefficients[ in_last_scanline - in_first_scanline ] == 0.0f ) )
      --in_last_scanline;

    // make sure the indexing hasn't broken
    STBIR_ASSERT( in_first_scanline >= split_info->ring_buffer_first_scanline );

    // Load in new scanlines
    while (in_last_scanline > split_info->ring_buffer_last_scanline)
    {
      STBIR_ASSERT( ( split_info->ring_buffer_last_scanline - split_info->ring_buffer_first_scanline + 1 ) <= stbir_info->ring_buffer_num_entries );

      // make sure there was room in the ring buffer when we add new scanlines
      if ( ( split_info->ring_buffer_last_scanline - split_info->ring_buffer_first_scanline + 1 ) == stbir_info->ring_buffer_num_entries )
      {
        split_info->ring_buffer_first_scanline++;
        split_info->ring_buffer_begin_index++;
      }

      co_yield split_info->ring_buffer_last_scanline + 1;
      if ( stbir_info->vertical_first )
      {
        float * ring_buffer = stbir__get_ring_buffer_scanline( stbir_info, split_info, ++split_info->ring_buffer_last_scanline );
        // Decode the nth scanline from the source image into the decode buffer.
        stbir__decode_scanline( stbir_info, split_info->ring_buffer_last_scanline, ring_buffer, slice  STBIR_ONLY_PROFILE_SET_SPLIT_INFO );
      }
      else
      {
        stbir__decode_and_resample_for_vertical_gather_loop(stbir_info, split_info, split_info->ring_buffer_last_scanline + 1, slice);
      }
    }

    // Now all buffers should be ready to write a row of vertical sampling, so do it.
    stbir__resample_vertical_gather(stbir_info, split_info, y, in_first_scanline, in_last_scanline, vertical_coefficients, slice );
  }
}
static Task pause_stbir__vertical_scatter_loop( stbir__info const * stbir_info, stbir__per_split_info* split_info, int split_count, stbir__column_slice const * slice )
{
  int y, start_output_y, end_output_y, start_input_y, end_input_y;
  stbir__contributors* vertical_contributors = stbir_info->vertical.contributors;
  float const * vertical_coefficients = stbir_info->vertical.coefficients;
  stbir__handle_scanline_for_scatter_func * handle_scanline_for_scatter;
  void * scanline_scatter_buffer;
  void * scanline_scatter_buffer_end;
  int on_first_input_y, last_input_y;
  ptrdiff_t width_times_channels = stbir__vertical_width_times_channels( stbir_info, slice );

  STBIR_ASSERT( !stbir_info->vertical.is_gather );

  start_output_y = split_info->start_output_y;
  end_output_y = split_info[split_count-1].end_output_y;  // may do multiple split counts

  start_input_y = split_info->start_input_y;
  end_input_y = split_info[split_count-1].end_input_y;

  // adjust for starting offset start_input_y
  y = start_input_y + stbir_info->vertical.filter_pixel_margin;
  vertical_contributors += y ;
  vertical_coefficients += (size_t)stbir_info->vertical.coefficient_width * y;

  if ( stbir_info->vertical_first )
  {
    handle_scanline_for_scatter = stbir__horizontal_resample_and_encode_first_scanline_from_scatter;
    scanline_scatter_buffer = split_info->decode_buffer;
  }
  else
  {
    handle_scanline_for_scatter = stbir__encode_first_scanline_from_scatter;
    scanline_scatter_buffer = split_info->vertical_buffer;
  }
  scanline_scatter_buffer_end = ( (char*) scanline_scatter_buffer ) + sizeof( float ) * width_times_channels;

  // initialize the ring buffer for scattering
  split_info->ring_buffer_first_scanline = start_output_y;
  split_info->ring_buffer_last_scanline = -1;
  split_info->ring_buffer_begin_index = -1;

  // mark all the buffers as empty to start
  for( y = 0 ; y < stbir_info->ring_buffer_num_entries ; y++ )
  {
    float * decode_buffer = stbir__get_ring_buffer_entry( stbir_info, split_info, y );
    decode_buffer[ width_times_channels ] = 0.0f; // clear two over for horizontals with a remnant of 3
    decode_buffer[ width_times_channels+1 ] = 0.0f; 
    decode_buffer[0] = STBIR__FLOAT_EMPTY_MARKER; // only used on scatter
  }

  // do the loop in input space
  on_first_input_y = 1;
  last_input_y = start_input_y;
  for (y = start_input_y ; y < end_input_y; y++)
  {
    int out_first_scanline, out_last_scanline;

    out_first_scanline = vertical_contributors->n0;
    out_last_scanline = vertical_contributors->n1;

    // this input row is ours if the output rows it contributes to overlap the split's rows at all
    if ( ( out_last_scanline >= out_first_scanline ) && ( out_last_scanline >= start_output_y ) && ( out_first_scanline < end_output_y ) )
    {
      float const * coefficients = vertical_coefficients;

      // keep track of the range actually seen for the next resize
      last_input_y = y;
      if ( ( on_first_input_y ) && ( y > start_input_y ) )
        split_info->start_input_y = y;
      on_first_input_y = 0;

      // clip the region
      if ( out_first_scanline < start_output_y )
      {
        coefficients += start_output_y - out_first_scanline;
        out_first_scanline = start_output_y;
      }

      if ( out_last_scanline >= end_output_y )
        out_last_scanline = end_output_y - 1;

      STBIR_ASSERT( out_last_scanline - out_first_scanline + 1 <= stbir_info->ring_buffer_num_entries );   // (the ring is capped to the band's rows, so after the clip)

      // if very first scanline, init the index
      if (split_info->ring_buffer_begin_index < 0)
        split_info->ring_buffer_begin_index = out_first_scanline - start_output_y;

      STBIR_ASSERT( split_info->ring_buffer_begin_index <= out_first_scanline );

      // Decode the nth scanline from the source image into the decode buffer.
      co_yield y;
      stbir__decode_scanline( stbir_info, y, split_info->decode_buffer, slice  STBIR_ONLY_PROFILE_SET_SPLIT_INFO );

      // When horizontal first, we resample horizontally into the vertical buffer before we scatter it out
      if ( !stbir_info->vertical_first )
        stbir__resample_horizontal_gather( stbir_info, split_info->vertical_buffer, split_info->decode_buffer, slice  STBIR_ONLY_PROFILE_SET_SPLIT_INFO );

      // Now it's sitting in the buffer ready to be distributed into the ring buffers.

      // evict from the ring buffer if it is full and this row needs a new entry
      if ( ( ( split_info->ring_buffer_last_scanline - split_info->ring_buffer_first_scanline + 1 ) == stbir_info->ring_buffer_num_entries ) &&
           ( out_last_scanline > split_info->ring_buffer_last_scanline ) )
        handle_scanline_for_scatter( stbir_info, split_info, slice );

      // Now the horizontal buffer is ready to write to all ring buffer rows, so do it.
      stbir__resample_vertical_scatter(stbir_info, split_info, out_first_scanline, out_last_scanline, coefficients, (float*)scanline_scatter_buffer, (float*)scanline_scatter_buffer_end );

      // update the end of the buffer
      if ( out_last_scanline > split_info->ring_buffer_last_scanline )
        split_info->ring_buffer_last_scanline = out_last_scanline;
    }
    ++vertical_contributors;
    vertical_coefficients += stbir_info->vertical.coefficient_width;
  }

  // now evict the scanlines that are left over in the ring buffer
  while ( split_info->ring_buffer_first_scanline < end_output_y )
    handle_scanline_for_scatter(stbir_info, split_info, slice);

  // update the end_input_y if we do multiple resizes with the same data
  ++last_input_y;
  for( y = 0 ; y < split_count; y++ )
    if ( split_info[y].end_input_y > last_input_y )
      split_info[y].end_input_y = last_input_y;
}
/*
------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
*/
