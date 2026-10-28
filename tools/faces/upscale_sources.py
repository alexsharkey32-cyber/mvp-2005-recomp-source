"""4x versions of the face generator's pieces (tools/faces/facegen.py) with Real-ESRGAN (realesr-general-x4v3, ONNX).
    python3 tools/faces/upscale_sources.py WORKDIR MODEL.onnx    ->  WORKDIR/faces_hd.npz"""
import sys, os, numpy as np, onnxruntime as ort, cv2
work, model = sys.argv[1], sys.argv[2]
z = np.load(os.path.join(work, 'faces.npz'))
s = ort.InferenceSession(model, providers=['CPUExecutionProvider']); inp = s.get_inputs()[0].name
def up(rgb):
    y = s.run(None, {inp: (rgb.astype(np.float32) / 255).transpose(2, 0, 1)[None]})[0][0].transpose(1, 2, 0)
    return np.clip(y * 255, 0, 255).astype(np.uint8)
def upa(a): return np.clip(cv2.resize(a.astype(np.float32), None, fx=4, fy=4, interpolation=cv2.INTER_CUBIC), 0, 1).astype(np.float16)
out = {'B': np.array([up(x) for x in z['B']]), 'D': np.array([up(x) for x in z['D']]),
       'A': np.array([cv2.resize(a, None, fx=4, fy=4, interpolation=cv2.INTER_NEAREST) for a in z['A']])}
hc = np.zeros((7, 10, 1024, 512, 3), np.uint8); bc = np.zeros((7, 8, 1024, 512, 3), np.uint8)
ha = np.zeros((7, 10, 1024, 512), np.float16); ba = np.zeros((7, 8, 1024, 512), np.float16)
for c in range(7):
    for k in range(1, 10): hc[c, k] = up(z['hc'][c, k]); ha[c, k] = upa(z['ha'][c, k])
    for k in range(1, 8): bc[c, k] = up(z['bc'][c, k]); ba[c, k] = upa(z['ba'][c, k])
    print('colour', c, flush=True)
out.update(hc=hc, bc=bc, ha=ha, ba=ba, clean=z['clean'])
np.savez(os.path.join(work, 'faces_hd.npz'), **out)
print('done')
