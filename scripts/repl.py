from bindings.python.lib import c_ray
import os.path
import datetime
import signal

try:
	import numpy as np
	import matplotlib.pyplot as plt
except ImportError:
	def show(_res):
		print("show() requires numpy and matplotlib, sorry!")

	def help_show():
		pass
else:
	def show(res):
		img = np.ndarray(shape=(res.height,res.width,4),
		                 dtype=np.float32,
						 buffer=res.data,
		            	 strides=(16*res.width,16,4))
		plt.imshow(img, cmap='gray')
		plt.show()

	def help_show():
		print("show(res) - show a rendered image (from renderer.get_result()) in a window")

def dump(obj):
	for attr in dir(obj):
		if hasattr(obj, attr):
			print("obj.{} = {}".format(attr, getattr(obj, attr)))

def help_dump():
	print("dump(obj) - show attributes of a thing at runtime")

def help():
	help_show()
	help_dump()

active_renderers = []

def _render(renderer, orig):
	active_renderers.append(renderer)
	orig()
	active_renderers.remove(renderer)

def cr_new(scene, skip_preview=False):
	# FIXME: actually just don't pass scene in this case...
	if not os.path.isfile(scene):
		print("{} doesn't exist".format(scene))
		return
	r = c_ray.renderer(scene)
	# override the render method with a wrapper so we can keep track of
	# where to send stop on ^C
	r.callbacks.on_start = (_on_start, None)
	r.callbacks.on_stop = (_on_stop, r)
	r.callbacks.on_status_update = (_on_status_update, None)
	r.render = lambda renderer=r, orig=r.render : _render(renderer, orig)
	r.skip_preview = skip_preview
	return r

def tldr():
	print("r = cr_new('input/hdr.json')")
	print("r.pref.samples = 100")
	print("r.render()")
	print("res = r.get_result()")
	print("# show res in a window (requires numpy & matplotlib)")
	print("show(res)")

def _on_start(_cb_info, _args):
	print("Render started")

def _on_stop(_cb_info, args):
	r = args
	if not r.skip_preview:
		res = r.get_result()
		show(res)
	r.skip_preview = False

def _on_status_update(cb_info, _args):
	print('\r[{:>3.0f}%] avg {:.2f}μs/ray ETA: {}'.format(cb_info.completion*100, cb_info.avg_per_ray, str(datetime.timedelta(milliseconds=cb_info.eta_ms))),
	      end='',
	      flush=True)

def render(scene, samples=250, skip_preview=False):
	r = cr_new(scene, skip_preview)
	r.prefs.samples = samples
	r.render()

def _sigint(_sig, _frame):
	if not active_renderers:
		exit(0)
	for r in active_renderers:
		r.skip_preview = True
		r.stop()
	
signal.signal(signal.SIGINT, _sigint)
print('c-ray v{} ({})'.format(c_ray.get_version(), c_ray.get_git_hash()))
print('call tldr() to see simple example of how to render a scene.')
print('call help() to list available helper functions')
