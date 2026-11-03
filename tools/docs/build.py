"""Renders the release docs with Microsoft Edge (headless):

  docs/images/banner.png                      GitHub page banner
  packaging/windows/wizard-image.bmp          installer side image
  packaging/windows/wizard-small.bmp          installer corner image
  docs/PitchNet-StarsClique-Manual-EN.pdf     user manual, English
  docs/PitchNet-StarsClique-Manual-RU.pdf     user manual, Russian

Run from anywhere:  python tools/docs/build.py
Pages reference fonts as url("font:NAME") and images as "img:NAME"; both are
inlined here as data URIs so Edge needs no network or file access.
"""
import base64
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
FONT_DIRS = [os.path.join(ROOT, 'Resources', 'fonts')]
IMAGE_DIRS = [os.path.join(ROOT, 'docs', 'images'), os.path.join(ROOT, 'Resources', 'images')]
EDGE_CANDIDATES = [
    r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
    r'C:\Program Files\Microsoft\Edge\Application\msedge.exe',
]
MIME = {'.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml',
        '.ttf': 'font/ttf', '.otf': 'font/otf'}


def find(name, dirs):
    for d in dirs:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    raise FileNotFoundError(name)


def data_uri(path):
    ext = os.path.splitext(path)[1].lower()
    with open(path, 'rb') as f:
        return 'data:%s;base64,%s' % (MIME[ext], base64.b64encode(f.read()).decode('ascii'))


def inline(html):
    # Stylesheets next to the page.
    def css(m):
        with open(os.path.join(HERE, m.group(1)), encoding='utf-8') as f:
            return '<style>\n' + f.read() + '\n</style>'
    html = re.sub(r'<link rel="stylesheet" href="([^"]+)">', css, html)
    html = re.sub(r'font:([\w.\-]+)', lambda m: data_uri(find(m.group(1), FONT_DIRS)), html)
    html = re.sub(r'img:([\w.\-]+)', lambda m: data_uri(find(m.group(1), IMAGE_DIRS)), html)
    return html


def edge():
    for p in EDGE_CANDIDATES:
        if os.path.isfile(p):
            return p
    sys.exit('Microsoft Edge not found')


def render(page, out, mode, size=None):
    with open(os.path.join(HERE, page), encoding='utf-8') as f:
        html = inline(f.read())
    tmpdir = tempfile.mkdtemp(prefix='pndocs-')
    try:
        src = os.path.join(tmpdir, 'page.html')
        with open(src, 'w', encoding='utf-8') as f:
            f.write(html)
        args = [edge(), '--headless=new', '--disable-gpu', '--no-first-run',
                '--user-data-dir=' + os.path.join(tmpdir, 'profile'),
                '--hide-scrollbars', '--run-all-compositor-stages-before-draw',
                '--virtual-time-budget=4000']
        if mode == 'pdf':
            args += ['--no-pdf-header-footer', '--print-to-pdf=' + out]
        else:
            args += ['--screenshot=' + out, '--window-size=%d,%d' % size,
                     '--force-device-scale-factor=1']
        args.append('file:///' + src.replace('\\', '/'))
        subprocess.run(args, check=True, timeout=180,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    print('wrote', out)


def png_to_bmp(png, bmp, size):
    # Inno Setup wants BMP; PowerShell's System.Drawing does the conversion.
    ps = ("Add-Type -AssemblyName System.Drawing;"
          "$i=[System.Drawing.Image]::FromFile('%s');"
          "$b=New-Object System.Drawing.Bitmap $i,%d,%d;"
          "$b.Save('%s',[System.Drawing.Imaging.ImageFormat]::Bmp);$b.Dispose();$i.Dispose()"
          % (png, size[0], size[1], bmp))
    subprocess.run([os.path.join(os.environ.get('SystemRoot', 'C:\\Windows'), 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe'), '-NoProfile', '-Command', ps], check=True)
    print('wrote', bmp)


def main():
    images = os.path.join(ROOT, 'docs', 'images')
    win = os.path.join(ROOT, 'packaging', 'windows')
    tmp = tempfile.mkdtemp(prefix='pnimg-')
    only = set(sys.argv[1:])
    try:
        if not only or 'banner' in only:
            render('banner.html', os.path.join(images, 'banner.png'), 'png', (1600, 560))
        if not only or 'wizard' in only:
            big = os.path.join(tmp, 'wizard.png')
            render('wizard.html', big, 'png', (328, 628))
            png_to_bmp(big, os.path.join(win, 'wizard-image.bmp'), (328, 628))
            small = os.path.join(tmp, 'small.png')
            render('wizard-small.html', small, 'png', (110, 110))
            png_to_bmp(small, os.path.join(win, 'wizard-small.bmp'), (110, 110))
        for lang in ('en', 'ru'):
            if not only or lang in only:
                render('manual-%s.html' % lang,
                       os.path.join(ROOT, 'docs', 'PitchNet-StarsClique-Manual-%s.pdf' % lang.upper()),
                       'pdf')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
