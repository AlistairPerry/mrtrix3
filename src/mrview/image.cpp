/*
    Copyright 2008 Brain Research Institute, Melbourne, Australia

    Written by J-Donald Tournier, 27/06/08.

    This file is part of MRtrix.

    MRtrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MRtrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <QMenu>

#include "progressbar.h"
#include "dataset/stride.h"
#include "mrview/image.h"
#include "mrview/window.h"
#include "mrview/mode/base.h"



namespace MR {
  namespace Viewer {

    Image::Image (Window& parent, MR::Image::Header* header) :
      QAction (shorten(header->name(), 20, 0).c_str(), &parent),
      H (*header),
      vox (H),
      interp (vox),
      window (parent),
      texture3D (0),
      interpolation (GL_LINEAR),
      value_min (NAN),
      value_max (NAN),
      display_midpoint (NAN),
      display_range (NAN),
      position (vox.ndim())
    {
      assert (header);
      setCheckable (true);
      setToolTip (header->name().c_str());
      setStatusTip (header->name().c_str());
      window.image_group->addAction (this);
      window.image_menu->addAction (this);
      texture2D[0] = texture2D[1] = texture2D[2] = 0;
      position[0] = position[1] = position[2] = std::numeric_limits<ssize_t>::min();
      set_colourmap (0, false, false);
    }

    Image::~Image () 
    {
      glDeleteTextures (3, texture2D);
      glDeleteTextures (1, &texture3D);
    }



    void Image::render2D (Shader& shader, int projection, int slice)
    {
      update_texture2D (projection, slice);

      glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, interpolation);
      glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, interpolation);

      int x, y;
      get_axes (projection, x, y);
      float xdim = H.dim(x)-0.5, ydim = H.dim(y)-0.5;

      Point<> p, q;
      p[projection] = slice;

      shader.start();

      shader.get_uniform ("offset") = display_midpoint - 0.5f * display_range;
      shader.get_uniform ("scale") = 1.0f / display_range;

      glBegin (GL_QUADS);
      glTexCoord2f (0.0, 0.0); p[x] = -0.5; p[y] = -0.5; q = interp.voxel2scanner (p); glVertex3fv (q);
      glTexCoord2f (0.0, 1.0); p[x] = -0.5; p[y] = ydim; q = interp.voxel2scanner (p); glVertex3fv (q);
      glTexCoord2f (1.0, 1.0); p[x] = xdim; p[y] = ydim; q = interp.voxel2scanner (p); glVertex3fv (q);
      glTexCoord2f (1.0, 0.0); p[x] = xdim; p[y] = -0.5; q = interp.voxel2scanner (p); glVertex3fv (q);
      glEnd();
      shader.stop();
    }





    void Image::render3D (Shader& shader, const Mode::Base& mode)
    {
      update_texture3D ();

      glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, interpolation);
      glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, interpolation);

      shader.start();

      shader.get_uniform ("offset") = (display_midpoint - 0.5f * display_range) / windowing_scale_3D;
      shader.get_uniform ("scale") = windowing_scale_3D / display_range;

      Point<> pos[4];
      pos[0] = mode.screen_to_model (QPoint (0, mode.height()));
      pos[1] = mode.screen_to_model (QPoint (0, 0));
      pos[2] = mode.screen_to_model (QPoint (mode.width(), 0));
      pos[3] = mode.screen_to_model (QPoint (mode.width(), mode.height()));

      Point<> tex[4];
      tex[0] = interp.scanner2voxel (pos[0]) + Point<> (0.5, 0.5, 0.5);
      tex[1] = interp.scanner2voxel (pos[1]) + Point<> (0.5, 0.5, 0.5);
      tex[2] = interp.scanner2voxel (pos[2]) + Point<> (0.5, 0.5, 0.5);
      tex[3] = interp.scanner2voxel (pos[3]) + Point<> (0.5, 0.5, 0.5);

      for (size_t i = 0; i < 4; ++i)
        for (size_t j = 0; j < 3; ++j)
          tex[i][j] /= H.dim(j);

      glBegin (GL_QUADS);
      glTexCoord3fv (tex[0]); glVertex3fv (pos[0]);
      glTexCoord3fv (tex[1]); glVertex3fv (pos[1]);
      glTexCoord3fv (tex[2]); glVertex3fv (pos[2]);
      glTexCoord3fv (tex[3]); glVertex3fv (pos[3]);
      glEnd();
      shader.stop();
    }




    inline void Image::update_texture2D (int projection, int slice)
    {
      if (!texture2D[projection]) { // allocate:
        glGenTextures (1, &texture2D[projection]);
        assert (texture2D[projection]);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
      }
      glBindTexture (GL_TEXTURE_2D, texture2D[projection]);

      if (position[projection] == slice && volume_unchanged()) 
        return;

      position[projection] = slice;

      int x, y;
      get_axes (projection, x, y);
      ssize_t xdim = H.dim(x), ydim = H.dim(y);
      float data [xdim*ydim];

      type = GL_FLOAT;
      format = GL_LUMINANCE;
      internal_format = GL_LUMINANCE32F_ARB;

      if (position[projection] < 0 || position[projection] >= H.dim(projection)) {
        memset (data, 0, xdim*ydim*sizeof(float));
      }
      else {
        // copy data:
        vox[projection] = slice;
        value_min = std::numeric_limits<float>::infinity();
        value_max = -std::numeric_limits<float>::infinity();
        for (vox[y] = 0; vox[y] < ydim; ++vox[y]) {
          for (vox[x] = 0; vox[x] < xdim; ++vox[x]) {
            float val = vox.value();
            data[vox[x]+vox[y]*xdim] = val;
            if (finite (val)) {
              if (val < value_min) value_min = val;
              if (val > value_max) value_max = val;
            }
          }
        }

        if (isnan (display_midpoint) || isnan (display_range))
          reset_windowing();
      }

      glTexImage2D (GL_TEXTURE_2D, 0, internal_format, xdim, ydim, 0, format, type, data);
    }





    inline void Image::update_texture3D ()
    {

      if (!texture3D) { // allocate:
        GLenum internal_format = GL_LUMINANCE32F_ARB;
        switch (vox.datatype()()) {
          case DataType::Bit: 
          case DataType::UInt8:
          case DataType::Int8:
            internal_format = GL_LUMINANCE8;
            break;
          case DataType::UInt16LE:
          case DataType::UInt16BE:
          case DataType::Int16LE:
          case DataType::Int16BE:
            internal_format = GL_LUMINANCE16;
            break;
          case DataType::UInt32LE:
          case DataType::UInt32BE:
          case DataType::Int32LE:
          case DataType::Int32BE:
          case DataType::Float32LE:
          case DataType::Float32BE:
          case DataType::Float64LE:
          case DataType::Float64BE:
            internal_format = GL_LUMINANCE32F_ARB;
            break;
          default:
            assert (0);
        }

        glGenTextures (1, &texture3D);
        assert (texture3D);
        glBindTexture (GL_TEXTURE_3D, texture3D);
        glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP);
        glTexImage3D (GL_TEXTURE_3D, 0, internal_format, 
            vox.dim(0), vox.dim(1), vox.dim(2), 
            0, GL_LUMINANCE, GL_FLOAT, NULL);
        DEBUG_OPENGL;
      }
      else if (volume_unchanged()) {
        glBindTexture (GL_TEXTURE_3D, texture3D);
        return;
      }

      glBindTexture (GL_TEXTURE_3D, texture3D);

      GLenum format = GL_LUMINANCE;
      value_min = std::numeric_limits<float>::infinity();
      value_max = -std::numeric_limits<float>::infinity();

      switch (vox.datatype()()) {
        case DataType::Bit: 
        case DataType::UInt8:
          copy_texture_3D<uint8_t> (format);
          break;
        case DataType::Int8:
          copy_texture_3D<int8_t> (format);
          break;
        case DataType::UInt16LE:
        case DataType::UInt16BE:
          copy_texture_3D<uint16_t> (format);
          break;
        case DataType::Int16LE:
        case DataType::Int16BE:
          copy_texture_3D<int16_t> (format);
          break;
        case DataType::UInt32LE:
        case DataType::UInt32BE:
          copy_texture_3D<uint32_t> (format);
          break;
        case DataType::Int32LE:
        case DataType::Int32BE:
          copy_texture_3D<int32_t> (format);
          break;
        case DataType::Float32LE:
        case DataType::Float32BE:
        case DataType::Float64LE:
        case DataType::Float64BE:
          copy_texture_3D<float> (format);
          break;
        default:
          assert (0);
      }

      if (isnan (display_midpoint) || isnan (display_range))
        reset_windowing();
    }

    template <> inline GLenum Image::GLtype<int8_t> () const { return (GL_BYTE); }
    template <> inline GLenum Image::GLtype<uint8_t> () const { return (GL_UNSIGNED_BYTE); }
    template <> inline GLenum Image::GLtype<int16_t> () const { return (GL_SHORT); }
    template <> inline GLenum Image::GLtype<uint16_t> () const { return (GL_UNSIGNED_SHORT); }
    template <> inline GLenum Image::GLtype<int32_t> () const { return (GL_INT); }
    template <> inline GLenum Image::GLtype<uint32_t> () const { return (GL_UNSIGNED_INT); }
    template <> inline GLenum Image::GLtype<float> () const { return (GL_FLOAT); }

    template <typename T> inline float Image::scale_factor_3D () const { return (std::numeric_limits<T>::max()); }
    template <> inline float Image::scale_factor_3D<float> () const { return (1.0); }


    template <typename T> inline void Image::copy_texture_3D (GLenum format) {

      MR::Image::Voxel<T> V (vox);
      GLenum type = GLtype<T>();
      Ptr<T,true> data (new T [V.dim(0)*V.dim(1)]);

      ProgressBar progress ("loading image data...", V.dim(2));
      for (V[2] = 0; V[2] < V.dim(2); ++V[2]) {
        T* p = data;
        for (V[1] = 0; V[1] < V.dim(1); ++V[1]) {
          for (V[0] = 0; V[0] < V.dim(0); ++V[0]) {
            T val = *p = V.value();
            if (finite (val)) {
              if (val < value_min) value_min = val;
              if (val > value_max) value_max = val;
            }
            ++p;
          }
        }
        glTexSubImage3D (GL_TEXTURE_3D, 0, 
            0, 0, V[2],
            V.dim(0), V.dim(1), 1,
            format, type, data);
        DEBUG_OPENGL;
        ++progress;
      }

      windowing_scale_3D = scale_factor_3D<T>(); 
    }



    inline bool Image::volume_unchanged ()
    {
      for (size_t i = 3; i < vox.ndim(); ++i)
        if (vox[i] != position[i]) 
          return false;
      return true;
    }



  }
}



