# Third-party notices

## ViGEmClient

`third_party/ViGEmClient` is a git submodule of
[nefarius/ViGEmClient](https://github.com/nefarius/ViGEmClient), compiled into DS4-Native to talk to
the ViGEmBus driver. Its license is in that folder.

## ViGEmBus and HidHide (bundled in the installer)

The setup carries the unmodified, signed installers of
[ViGEmBus](https://github.com/nefarius/ViGEmBus) 1.22.0 (BSD-3-Clause) and
[HidHide](https://github.com/nefarius/HidHide) 1.5.230 (MIT), and runs them only when the driver
isn't installed. Their licenses:

ViGEmBus:

```
BSD 3-Clause License

Copyright (c) 2016-2020, Nefarius Software Solutions e.U.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

HidHide:

```
MIT License

Copyright (c) 2020 Eric Korff de Gidts
Copyright (c) 2021-2024 Benjamin Höglinger-Stelzer

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## DS4-Sound-Link

The SBC encoder in `app/DS4Native.cpp` (`audio::Sbc`) is a C++ port of `DualShock4SbcEncoder` and
`SbcEncoderTables` from [DS4-Sound-Link](https://github.com/abh1shekbana/DS4-Sound-Link), and the
layout of the DualShock 4's Bluetooth sound reports (0x11 arming, 0x17 audio) follows its protocol
notes.

```
MIT License

Copyright (c) 2026 ControllerAudioSwitcher Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Protocol references (no code taken)

- [PadForge issue #528](https://github.com/hifihedgehog/PadForge/issues/528) and the notes it cites: the layout
  of the DualShock 4's Bluetooth microphone reports and how the mic is switched on.
- [OpenChromaConnect/ChromaBroadcastAPI](https://github.com/OpenChromaConnect/ChromaBroadcastAPI):
  the Chroma Broadcast callback types and Razer's sample App ID.
- Razer's Chroma Connect module (`RzChromaBroadcastAPI64.dll`) is loaded from the user's own Razer
  Synapse installation at run time; it isn't included.
