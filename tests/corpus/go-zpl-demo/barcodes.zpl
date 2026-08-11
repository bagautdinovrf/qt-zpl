^XA

^FX Barcode Sampler - shows supported barcode types
^CF0,35
^FO250,20^FDBarcode Sampler^FS

^FX Code 128
^CF0,20
^FO50,70^FDCode 128:^FS
^BY2,2,80
^FO50,95^BC^FDCODE128-TEST^FS

^FX QR Code
^FO500,70^FDQR Code:^FS
^FO500,95^BQN,2,4^FDMM,Ahttps://go-zpl.dev^FS

^FX DataMatrix
^FO50,250^FDDataMatrix:^FS
^FO50,275^BXN,6,200^FDDataMatrix 123^FS

^FX MaxiCode
^FO450,250^FDMaxiCode:^FS
^FO420,275^BD2^FH_^FD000000000000000[)>_1E01_1D961Z00000001_1DUPSN_1D12345_1E07TESTDATA_0D_1E_04^FS

^FX PDF417 - on its own row since it's wide
^FO50,480^FDPDF417:^FS
^FO50,505^B7N,50,2^FDPDF417 Demo^FS

^FX Note about other barcodes
^CF0,16
^FO50,620^FDSupported: Code 128, QR, DataMatrix, PDF417, MaxiCode^FS
^FO50,645^FDComing soon: Code 39, UPC-A, EAN-13, Interleaved 2of5^FS

^XZ