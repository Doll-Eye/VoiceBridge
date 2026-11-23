# Third-party material

## NVDA controller interface

`src/front/controller.idl` holds the `NvdaController` and `NvdaController3` declarations from NVDA's `nvdaHelper/interfaces/nvdaController/nvdaController.idl`, so that VoiceBridge answers exactly as NVDA does.

- Copyright 2006-2026 NV Access Limited, Leonard de Ruijter, Ethin Probst.
- Licence: GNU Lesser General Public License, version 2.1 only (SPDX `LGPL-2.1-only`). The full text is in [LICENSES/LGPL-2.1-only.txt](LICENSES/LGPL-2.1-only.txt).
- Changed for VoiceBridge in November 2026: only these two interfaces are kept, and a binding attribute was added for WIDL.
- Source: <https://github.com/nvaccess/nvda>

The RPC stubs that `build.sh` makes from this file are generated at build time and are not kept in this repository.

## Used but not included

- NV Access's controller client DLL and PRISM 0.18.3 are used by the tests. They are not part of this repository.
- NaturalVoiceSAPIAdapter, which makes the natural voices available through SAPI, is installed separately.
