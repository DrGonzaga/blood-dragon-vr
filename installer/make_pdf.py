"""Makes the controls PDF (one page per picture) that Setup opens and the release zip carries.
The PNGs go in losslessly (img2pdf), so the neon text stays sharp.
Usage: python3 make_pdf.py <pictures folder> <output pdf>      (pip install img2pdf)"""
import sys, os
import img2pdf
pics, out = sys.argv[1:3]
order = ["controls-controllers.png", "controls-body.png", "controls-tutorial.png"]
files = [os.path.join(pics, n) for n in order if os.path.exists(os.path.join(pics, n))]
if not files:
    sys.exit("no controls pictures in " + pics)
# 150 dpi: a 1920 px wide picture becomes a 32.5 cm page, which viewers fit to the window
with open(out, "wb") as f:
    f.write(img2pdf.convert(files, layout_fun=img2pdf.get_fixed_dpi_layout_fun((150, 150)),
                            title="Blood Dragon VR controls", author="DrGonzaga"))
print("controls pdf:", out, len(files), "pages")
