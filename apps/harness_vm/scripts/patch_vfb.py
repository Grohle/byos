import sys,re
p=sys.argv[1]; s=open(p).read()
if 'HIBY' in s: print('already'); sys.exit()
s=s.replace('#define VIDEOMEMSIZE	(1*1024*1024)	/* 1 MB */','#define VIDEOMEMSIZE	(320*960*2)	/* HIBY R3 II: 320x960x16 */')
s=s.replace('''	.xres =		640,
	.yres =		480,''','''	.xres =		320,	/* HIBY */
	.yres =		480,''')
s=s.replace('.id =		"Virtual FB",','.id =		"hiby-vfb",')
# RGB565 like real LCD controllers: R at 11, B at 0
s=s.replace('''		} else {	/* RGB 565 */
			var->red.offset = 0;
			var->red.length = 5;
			var->green.offset = 5;
			var->green.length = 6;
			var->blue.offset = 11;''','''		} else {	/* RGB 565 (HIBY: R=11,G=5,B=0) */
			var->red.offset = 11;
			var->red.length = 5;
			var->green.offset = 5;
			var->green.length = 6;
			var->blue.offset = 0;''')
s=s.replace('''			  NULL, 0, &vfb_default, 8)){''','''			  NULL, 0, &vfb_default, 16)){''')
s=s.replace('''	vfb_fix.smem_start = (unsigned long) videomemory;''','''	/* HIBY: start like the device: 320x480 visible, 320x960 virtual, RGB565 */
	info->var.bits_per_pixel = 16;
	info->var.xres_virtual = 320;
	info->var.yres_virtual = 960;
	info->var.red.offset = 11; info->var.red.length = 5;
	info->var.green.offset = 5; info->var.green.length = 6;
	info->var.blue.offset = 0; info->var.blue.length = 5;
	info->var.transp.offset = 0; info->var.transp.length = 0;
	vfb_fix.smem_start = (unsigned long) videomemory;''')
# default enabled
s=s.replace('static bool vfb_enable __initdata = 0;	/* disabled by default */','static bool vfb_enable __initdata = 1;	/* HIBY: enabled by default */')
open(p,'w').write(s); print('patched', s.count('HIBY'))
