/*
  SDLop - minimal stand-in for SDL3's SDL_begin_code.h so the headers are
  self-contained. SDLop exports no symbols with custom decoration; the
  macro exists so code written against SDL3 keeps compiling.

  This software is provided 'as-is', without any express or implied
  warranty (zlib license, see LICENSE).
*/

#ifndef SDL_begin_code_h
#define SDL_begin_code_h

#ifndef SDL_DECLSPEC
#define SDL_DECLSPEC
#endif

#ifndef SDLCALL
#define SDLCALL
#endif

#endif /* SDL_begin_code_h */
