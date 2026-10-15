"""Linear-elastic FEA of the west rim pulled outward.

Floor underside fixed. The rim's inner face (the seat band, x = -POCKET_CLEAR)
over the keys' path is moved u_x = -D. Reports peak principal strain (and
where), and the force it takes. P2 tets on a gmsh mesh, scikit-fem.
Needs gmsh, scikit-fem, meshio (not in enclosure/requirements.txt):
    python3 -m venv fea && fea/bin/pip install gmsh scikit-fem meshio
    fea/bin/python fea.py ../../../enclosure/out/airwriter-tray.step 2.0 0.8 2000 1.2
usage: fea.py tray.step D(mm) h_fine(mm) E(MPa) [WEST_FLEX_T]"""
import sys, time
import numpy as np
import gmsh
from skfem import MeshTet, ElementTetP2, ElementVector, Basis, asm, condense, solve
from skfem.models.elasticity import linear_elasticity, lame_parameters

step, D, h, E = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
NU, PC = 0.38, 0.30
Y0, Y1 = 19.0, 42.0          # where the hand pulls
K0, K1 = 20.0, 40.5          # keys' path at the rim (A-phase rock-in)

t0 = time.time()
gmsh.initialize(); gmsh.option.setNumber("General.Terminal", 0)
gmsh.model.occ.importShapes(step); gmsh.model.occ.synchronize()
f = gmsh.model.mesh.field
f.add("Box", 1)
for k, v in dict(VIn=h, VOut=3.0, XMin=-3.5, XMax=3.0, YMin=-4, YMax=112, ZMin=-8, ZMax=15, Thickness=4).items():
    f.setNumber(1, k, v)
f.add("Box", 2)   # front wall near the west corner too (the corner post + J2 lintel)
for k, v in dict(VIn=h, VOut=3.0, XMin=-3.5, XMax=22, YMin=-3.5, YMax=3.0, ZMin=-8, ZMax=15, Thickness=4).items():
    f.setNumber(2, k, v)
f.add("Min", 3); f.setNumbers(3, "FieldsList", [1, 2]); f.setAsBackgroundMesh(3)
for o in ("Mesh.MeshSizeExtendFromBoundary", "Mesh.MeshSizeFromPoints", "Mesh.MeshSizeFromCurvature"):
    gmsh.option.setNumber(o, 0)
gmsh.model.mesh.generate(3)
tags, xyz, _ = gmsh.model.mesh.getNodes()
pts = xyz.reshape(-1, 3)
idx = {t: i for i, t in enumerate(tags)}
et, en = gmsh.model.mesh.getElementsByType(4)
tets = np.vectorize(idx.get)(en.reshape(-1, 4))
gmsh.finalize()
used = np.unique(tets)
remap = -np.ones(len(pts), int); remap[used] = np.arange(len(used))
m = MeshTet(pts[used].T, remap[tets].T)
print(f"mesh {m.t.shape[1]} tets, {time.time()-t0:.0f}s", flush=True)

e = ElementVector(ElementTetP2())
ib = Basis(m, e, intorder=2)
lam, mu = lame_parameters(E, NU)
K = asm(linear_elasticity(lam, mu), ib)
print(f"assembled {K.shape[0]} dofs, {time.time()-t0:.0f}s", flush=True)

X = ib.doflocs  # (3, ndofs): location of each DOF; DOFs interleave u_x,u_y,u_z
nd = X.shape[1]
comp = np.arange(nd) % 3
floor = np.where(X[2] < -6.999)[0]
patch = np.where((np.abs(X[0] + PC) < 1e-3) & (X[2] > 13.04 - 1e-3) & (X[1] > Y0) & (X[1] < Y1) & (comp == 0))[0]
F = np.zeros(nd); F[patch] = -1.0 / len(patch)   # 1 N spread over the patch
u = solve(*condense(K, F, D=floor))
keys = np.where((np.abs(X[0] + PC) < 1e-3) & (X[2] > 13.04 - 1e-3) & (X[1] > K0) & (X[1] < K1) & (comp == 0))[0]
scale = D / (-u[keys]).min()          # pull until the WHOLE key path is out by D
u *= scale
print(f"solved, {time.time()-t0:.0f}s; patch dofs {len(patch)}", flush=True)

force = scale
ub = ib.interpolate(u)
g = ub.grad  # (3,3,nel,nqp)
eps = 0.5 * (g + np.swapaxes(g, 0, 1))
ev = np.linalg.eigvalsh(np.moveaxis(eps, (0, 1), (-2, -1)))  # (nel,nqp,3)
p1 = ev[..., -1].max(axis=1)     # max principal per element
p3 = ev[..., 0].min(axis=1)
c = m.p[:, m.t].mean(axis=1)
order = np.argsort(-p1)
print(f"D={D} h={h} E={E}: force {force:.1f} N")
print("top tensile-strain elements (strain%, x, y, z):")
seen = []
for i in order:
    loc = c[:, i]
    if any(np.linalg.norm(loc - s) < 4 for s in seen): continue
    seen.append(loc); print(f"  {100*p1[i]:.2f}%  ({loc[0]:.1f}, {loc[1]:.1f}, {loc[2]:.1f})")
    if len(seen) == 6: break
print(f"peak compressive {100*p3.min():.2f}%")
# Root zone: west wall within ~3.4 of the floor top -- where rev A's print split.
# eps_zz (vertical) is what pulls layers apart on a floor-down print.
ezz = eps[2, 2].max(axis=1)
root = (c[0] < -0.25) & (c[2] < -1.2) & (c[1] > 4) & (c[1] < 100)
i = np.argmax(np.where(root, ezz, -1))
print(f"ROOT: max eps_zz {100*ezz[i]:.2f}% at ({c[0,i]:.1f}, {c[1,i]:.1f}, {c[2,i]:.1f}); max principal {100*np.where(root, p1, 0).max():.2f}%")

# Outward displacement vs what the front key needs on the ramp/rim near y 18.5-20
# (keys' tips at x = -2.2 with the board centred; 0.05 margin).
ramp_x = lambda z: max(-p_xin, min(-PC, -p_xin + (z - z_rb)))
p_xin, z_rb = 3.0 - 0.8, 12.74 - (3.0 - 0.8 - PC)
P = []
for y in (18.5, 19.0, 19.5, 20.0):
    for z in (11.0, 11.6, 12.3, 13.0, 14.0):
        P.append((ramp_x(z) - 0.05, y, z))
P = np.array(P).T
sb = ib.split_bases()[0]
try:
    ux = sb.probes(P) @ u[ib.split_indices()[0]]
except ValueError:
    ux = np.full(P.shape[1], np.nan)
print("front-key clearance check (y, z, face x, outward u, needed):")
for (x_, y, z), d in zip(P.T, ux):
    need = (x_ + 0.05) + 2.2 + 0.05
    print(f"  y{y:5.1f} z{z:5.1f}  face {x_+0.05:6.2f}  u {-d:5.2f}  need {max(need,0):5.2f}  {'ok' if -d >= need else 'SHORT'}")
wall = (c[0] < -0.25) & (c[1] > 4) & (c[1] < 100)
win = (c[1] > 18) & (c[1] < 42) & (c[2] > 3.6) & (c[2] < 10.9)   # DIP window + corners
bands = [(-4.6, -2), (-2, 0), (0, 2), (2, 4), (4, 7), (7, 10), (10, 13.1)]
print("EZZ by z band (wall, excl. window):", "  ".join(
    f"{a:+.0f}..{b:+.0f}:{100*np.where(wall & ~win & (c[2]>=a) & (c[2]<b), ezz, 0).max():.2f}" for a, b in bands))
