## [1.17.1](https://github.com/corradoignoti/annota/compare/v1.17.0...v1.17.1) (2026-10-09)


### Bug Fixes

* **ui:** refresh Notes list after web-side file changes ([22d40a8](https://github.com/corradoignoti/annota/commit/22d40a80879631d8f31341d2af1c03c24d98300f))
* **web:** show transcription progress in the Sync bar at the top ([af80a86](https://github.com/corradoignoti/annota/commit/af80a8609ddd868988681f97019384677e4e65f5)), closes [#statusBox](https://github.com/corradoignoti/annota/issues/statusBox) [#syncBar](https://github.com/corradoignoti/annota/issues/syncBar)

# [1.17.0](https://github.com/corradoignoti/annota/compare/v1.16.0...v1.17.0) (2026-10-07)


### Features

* **397:** summary box on the sleep screen ([5183bd2](https://github.com/corradoignoti/annota/commit/5183bd2523f5a45cab75a13c87922e0b1adcb5ef))
* logo sleep screen on both boards ([7061b7d](https://github.com/corradoignoti/annota/commit/7061b7d5443f7559854d4c244844ea22f4eb57d9))
* **openai:** verify api.openai.com TLS certificate ([549c345](https://github.com/corradoignoti/annota/commit/549c3452db5a66328649737857553095981961a6))

# [1.16.0](https://github.com/corradoignoti/annota/compare/v1.15.1...v1.16.0) (2026-10-07)


### Features

* **397:** raise on-device transcription cap to 4 MB ([0b83858](https://github.com/corradoignoti/annota/commit/0b8385829c7d8deb88e52ed51ebcd63d76eb8a81))

## [1.15.1](https://github.com/corradoignoti/annota/compare/v1.15.0...v1.15.1) (2026-10-06)


### Bug Fixes

* **release:** give each firmware asset a unique upload name ([d810293](https://github.com/corradoignoti/annota/commit/d81029313953649ec84a6955a63e6af982d23b8f))

# [1.15.0](https://github.com/corradoignoti/annota/compare/v1.14.2...v1.15.0) (2026-10-06)


### Bug Fixes

* **397:** power down peripherals before deep sleep ([dde7cac](https://github.com/corradoignoti/annota/commit/dde7cac7decf8ccf885c77d882149af0f25b99de))
* battery icon clipped out of the header status bar ([fb61d21](https://github.com/corradoignoti/annota/commit/fb61d21e4400848e0b0854b89078b978999e92dd))
* don't block on Serial output after a firmware upload ([67cc42f](https://github.com/corradoignoti/annota/commit/67cc42f09d418bf1dab6def33dc2a8d0618d3a12))


### Features

* **397:** show AP join and web UI QR codes together ([bc400d3](https://github.com/corradoignoti/annota/commit/bc400d36375114ffdf074bad30d5b1c6cd1b96eb))
* **397:** volume menu during playback ([b1ee4f8](https://github.com/corradoignoti/annota/commit/b1ee4f8284697f4bc8896f88361c07a3e3369cce))
* add esp32-s3-epaper397 env for Waveshare ESP32-S3-ePaper-3.97 ([dbe393d](https://github.com/corradoignoti/annota/commit/dbe393d755c875813a936357802781d2fefd81b0))


### Performance Improvements

* **397:** faster screen changes on the 3.97 ([b6a862e](https://github.com/corradoignoti/annota/commit/b6a862e65f57b1e9afba40c2759dd32c70180f92))

## [1.14.2](https://github.com/corradoignoti/annota/compare/v1.14.1...v1.14.2) (2026-10-06)


### Bug Fixes

* battery icon clipped out of the header status bar ([65617c0](https://github.com/corradoignoti/annota/commit/65617c05fa2fd862c742855a4218c1f5cb73450f))

## [1.14.1](https://github.com/corradoignoti/annota/compare/v1.14.0...v1.14.1) (2026-10-06)


### Bug Fixes

* don't block on Serial output after a firmware upload ([298d8bb](https://github.com/corradoignoti/annota/commit/298d8bb79fbcd4e257d36ac4680cc2396ef20020))

# [1.14.0](https://github.com/corradoignoti/annota/compare/v1.13.0...v1.14.0) (2026-10-01)


### Features

* web GUI icons, About page, drop browser upload ([f401e47](https://github.com/corradoignoti/annota/commit/f401e47de64604376fbd8fc2d0b19c0710d6d498))

# [1.13.0](https://github.com/corradoignoti/annota/compare/v1.12.0...v1.13.0) (2026-10-01)


### Bug Fixes

* keep web file manager actions visible ([1d2e347](https://github.com/corradoignoti/annota/commit/1d2e3474e5097d2d9100d5cfb0da54681c69bf26))


### Features

* delete audio transcript along with audio file on device ([16362be](https://github.com/corradoignoti/annota/commit/16362bebc1f7527f94da89a517ee8c2baad2a77b))
* Notes view in the web file manager ([936b282](https://github.com/corradoignoti/annota/commit/936b282f79b38f808cf81712e495c4c8d7f76af8))
* rename audio list to Notes, drop on-device text list and per-file transfer ([16e922a](https://github.com/corradoignoti/annota/commit/16e922ab6cb2fb891b57ea829df84a300aff2f09))
* show "View transcription" instead of "Transcribe" for transcribed audio ([9fadf81](https://github.com/corradoignoti/annota/commit/9fadf8171581dab88ad61ba7e49d42809488d86b))
* show transcript details on audio Details screen ([f8aba4f](https://github.com/corradoignoti/annota/commit/f8aba4f437c408bf3b43b9fe4dc7db2d5b1d4f56))
* show transcript titles under audio files on the device list ([0347c9a](https://github.com/corradoignoti/annota/commit/0347c9a375ff124800ba4b310db81dfeb9b8c396))
* sort device audio list by transcript date, untranscribed first ([9e02562](https://github.com/corradoignoti/annota/commit/9e02562b0e469ad19df0704a24fb6281d471cbb3))

# [1.12.0](https://github.com/corradoignoti/annota/compare/v1.11.0...v1.12.0) (2026-10-01)


### Bug Fixes

* translate on-device "file too large to transcribe" message ([b3e63d6](https://github.com/corradoignoti/annota/commit/b3e63d6f9fe908f38955df2bc58ceb4c0ac5f870))


### Features

* web UI Sync button to transcribe all untranscribed audio ([2bab2ab](https://github.com/corradoignoti/annota/commit/2bab2abc33f58b7a3bc52cb388647fa86a21fa49))

# [1.11.0](https://github.com/corradoignoti/annota/compare/v1.10.1...v1.11.0) (2026-09-30)


### Bug Fixes

* web UI labels blank because i18n.js was malformed ([0ba8e01](https://github.com/corradoignoti/annota/commit/0ba8e01c595fb4288d6456fa2fc5af9b538567e7))


### Features

* translate device and web UI (English, Italian, French) ([f445b01](https://github.com/corradoignoti/annota/commit/f445b01297b48736d7480034595e8da4edfe8a16))

## [1.10.1](https://github.com/corradoignoti/annota/compare/v1.10.0...v1.10.1) (2026-09-30)


### Bug Fixes

* make on-device transcription size cap a build flag, raise to 2 MB ([0e6d4ce](https://github.com/corradoignoti/annota/commit/0e6d4ce2ebd16221852ab3367a87c7b55cedb316))

# [1.10.0](https://github.com/corradoignoti/annota/compare/v1.9.1...v1.10.0) (2026-09-30)


### Bug Fixes

* web Transcribe button reports skipped title/abstract ([25fa9b6](https://github.com/corradoignoti/annota/commit/25fa9b62e1d7dca38cf0abe5b9732aa9ae252f70)), closes [#26](https://github.com/corradoignoti/annota/issues/26)


### Features

* add AI title and abstract on top of transcripts ([84a0169](https://github.com/corradoignoti/annota/commit/84a016983cc56d7d4b5277bd60b58364b5951461))
* cap on-device transcription at 1 MB ([6af12c0](https://github.com/corradoignoti/annota/commit/6af12c0059ee7dac6bb1e89a3b2efe772e4990b0))
* show transcription phases in the web Transcribe flow ([fd2d737](https://github.com/corradoignoti/annota/commit/fd2d737f13db3683d3f0904c1c48a90d6bdb54d5))

## [1.9.1](https://github.com/corradoignoti/annota/compare/v1.9.0...v1.9.1) (2026-09-30)


### Bug Fixes

* transcription upload failing with HTTP -3 on long recordings ([36b87ba](https://github.com/corradoignoti/annota/commit/36b87ba6edaab23ee6e12b484e90773fd7f60b0b))

# [1.9.0](https://github.com/corradoignoti/annota/compare/v1.8.0...v1.9.0) (2026-09-29)


### Features

* USB drive mode exposing the SD card as mass storage ([c8698f8](https://github.com/corradoignoti/annota/commit/c8698f81210639121ee5521f99c76fd339f3b4e0))

# [1.8.0](https://github.com/corradoignoti/annota/compare/v1.7.0...v1.8.0) (2026-09-29)


### Features

* Details screen in file action menu ([392b78a](https://github.com/corradoignoti/annota/commit/392b78a60b1d9d5746e4d821461df6ed27a23ca8))

# [1.7.0](https://github.com/corradoignoti/annota/compare/v1.6.0...v1.7.0) (2026-09-29)


### Features

* transcription progress screen, upload bar, and failure log ([f89a529](https://github.com/corradoignoti/annota/commit/f89a529e8d02ce94c96142471e7ab68478386ce4))

# [1.6.0](https://github.com/corradoignoti/annota/compare/v1.5.1...v1.6.0) (2026-09-29)


### Features

* hold both buttons 5s to reboot, even when hung ([d3780ae](https://github.com/corradoignoti/annota/commit/d3780ae44742318e62285866acf248102081dcc6))

## [1.5.1](https://github.com/corradoignoti/annota/compare/v1.5.0...v1.5.1) (2026-09-23)


### Bug Fixes

* recalibrate battery percent curve, never reached 100% ([572bcc8](https://github.com/corradoignoti/annota/commit/572bcc80bf8e4512d2ff6d6bb1d0283c39945783))

# [1.5.0](https://github.com/corradoignoti/annota/compare/v1.4.1...v1.5.0) (2026-09-22)


### Features

* WiFi password show/hide toggle, drop Delete WiFi Setup, view .txt inline ([b887639](https://github.com/corradoignoti/annota/commit/b887639bc6133399d3682cea161ce22940a07b32))

## [1.4.1](https://github.com/corradoignoti/annota/compare/v1.4.0...v1.4.1) (2026-09-22)


### Bug Fixes

* don't show connection QR unless WiFi is really connected ([4ad0327](https://github.com/corradoignoti/annota/commit/4ad03273c883dc9366b283a947c2119a5c1f10b9))

# [1.4.0](https://github.com/corradoignoti/annota/compare/v1.3.0...v1.4.0) (2026-09-21)


### Features

* per-file "File transfer" link + QR code in file context menu ([7cf690c](https://github.com/corradoignoti/annota/commit/7cf690ce50d7419138c666fb23faedfd29713724))

# [1.3.0](https://github.com/corradoignoti/annota/compare/v1.2.0...v1.3.0) (2026-09-21)


### Features

* add Home screen (Audio/Text/File transfer carousel) ([882a642](https://github.com/corradoignoti/annota/commit/882a642fc7cc97a7e995efdce534a55be1e9db91))
* allow skipping WiFi setup portal to work offline ([c07681b](https://github.com/corradoignoti/annota/commit/c07681bb6cbcc8cfbdb87f1eb06b380d4b804c38))
* multi-AP WiFi list, web management, and on-device scan+join ([1f2819d](https://github.com/corradoignoti/annota/commit/1f2819dcf76e740e03185cae6120c56b719f2555))
* on-device .txt preview screen, accented-letter font support ([188e2fe](https://github.com/corradoignoti/annota/commit/188e2fed5303bd2923d88b2ff0b13d144682fa9a))
* on-device WiFi management screen (join AP / create AP) ([fb60831](https://github.com/corradoignoti/annota/commit/fb608316c1fca233cb44aecdca94659699e6be44))
* show a connected screen after joining a WiFi network on-device ([c5cf205](https://github.com/corradoignoti/annota/commit/c5cf2053e94937b674d22a2b5862b27a763a68ae))
* show a QR code on the standalone-AP screen ([8b7fa36](https://github.com/corradoignoti/annota/commit/8b7fa36a56d37595bfaf019f6cabca044415daa1))
* show a QR code on the WiFi-joined screen ([5d5e66c](https://github.com/corradoignoti/annota/commit/5d5e66c9e4742de1ef13ed1afb233e5e90e994ea))
* WiFi off by default, on-demand for on-device transcription ([072f086](https://github.com/corradoignoti/annota/commit/072f08615d5c66fa2716407da8c0019d0f00fe36))

# [1.2.0](https://github.com/corradoignoti/annota/compare/v1.1.1...v1.2.0) (2026-09-15)


### Features

* swap mbedTLS for wolfSSL, fix missing SNI breaking OpenAI transcription ([cd0a5b8](https://github.com/corradoignoti/annota/commit/cd0a5b8097d775aa426ccb2f2119f7dba7290fe7))

## [1.1.1](https://github.com/corradoignoti/annota/compare/v1.1.0...v1.1.1) (2026-09-06)


### Bug Fixes

* use piecewise Li-ion discharge curve for battery percent ([2dab5d8](https://github.com/corradoignoti/annota/commit/2dab5d8b5bae42b63def577dd588e1ce5eb79c04))
