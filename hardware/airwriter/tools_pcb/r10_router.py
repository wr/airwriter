import sys,math
sys.path.insert(0,'/Users/wells/Projects/airwriter/hardware/airwriter/tools_pcb')
import pcbnew, r6_lib as L
mm=lambda v:v/1e6; nm=L.nm
def vw(t):
    try:return mm(t.GetWidth(pcbnew.F_Cu))/2
    except:return 0.3
def obstacles(b,net,layer):
    pads=[];segs=[]
    for f in b.GetFootprints():
        for p in f.Pads():
            if p.GetNetname()==net or not p.IsOnLayer(layer):continue
            c=p.GetCenter();pads.append((mm(c.x),mm(c.y),max(mm(p.GetSize().x),mm(p.GetSize().y))/2))
    for t in b.GetTracks():
        if isinstance(t,pcbnew.PCB_VIA):
            if t.GetNetname()!=net:c=t.GetPosition();pads.append((mm(c.x),mm(c.y),vw(t)))
            continue
        if t.GetLayer()!=layer or t.GetNetname()==net:continue
        s,e=t.GetStart(),t.GetEnd();segs.append((mm(s.x),mm(s.y),mm(e.x),mm(e.y),mm(t.GetWidth())/2))
    return pads,segs
def spd(ax,ay,bx,by,px,py):
    dx,dy=bx-ax,by-ay;L2=dx*dx+dy*dy
    t=0 if L2==0 else max(0,min(1,((px-ax)*dx+(py-ay)*dy)/L2));return math.hypot(px-(ax+t*dx),py-(ay+t*dy))
def ssd(a,c):return min(spd(a[0],a[1],a[2],a[3],c[0],c[1]),spd(a[0],a[1],a[2],a[3],c[2],c[3]),spd(c[0],c[1],c[2],c[3],a[0],a[1]),spd(c[0],c[1],c[2],c[3],a[2],a[3]))
def clean(b,x0,y0,x1,y1,net,layer,w):
    pads,segs=obstacles(b,net,layer);need=w/2+0.16
    for px,py,pr in pads:
        if spd(x0,y0,x1,y1,px,py)<need+pr:return False
    for s in segs:
        if ssd((x0,y0,x1,y1),(s[0],s[1],s[2],s[3]))<need+s[4]:return False
    return True
def track(b,x0,y0,x1,y1,net,w,layer):
    t=pcbnew.PCB_TRACK(b);t.SetStart(pcbnew.VECTOR2I(nm(x0),nm(y0)));t.SetEnd(pcbnew.VECTOR2I(nm(x1),nm(y1)));t.SetWidth(nm(w));t.SetLayer(layer);t.SetNet(b.FindNet(net));b.Add(t)
def via(b,x,y,net):
    v=pcbnew.PCB_VIA(b);v.SetPosition(pcbnew.VECTOR2I(nm(x),nm(y)));v.SetWidth(nm(0.6));v.SetDrill(nm(0.3));v.SetLayerPair(pcbnew.F_Cu,pcbnew.B_Cu);v.SetNet(b.FindNet(net));b.Add(v)
def route_LZ(b,x0,y0,x1,y1,net,w):
    # F.Cu first (no vias). Then B.Cu with vias at both endpoints.
    if clean(b,x0,y0,x1,y1,net,pcbnew.F_Cu,w):track(b,x0,y0,x1,y1,net,w,pcbnew.F_Cu);return 'directF'
    for cx,cy in [(x1,y0),(x0,y1)]:
        if clean(b,x0,y0,cx,cy,net,pcbnew.F_Cu,w) and clean(b,cx,cy,x1,y1,net,pcbnew.F_Cu,w):
            track(b,x0,y0,cx,cy,net,w,pcbnew.F_Cu);track(b,cx,cy,x1,y1,net,w,pcbnew.F_Cu);return 'LF'
    # B.Cu with validated vias: find a clear via spot near each endpoint (clear on
    # both layers), clean F.Cu stub pad->via, clean B.Cu middle.
    import math as M
    def via_clear(vx,vy):
        pf,sf=obstacles(b,net,pcbnew.F_Cu); pb,sb=obstacles(b,net,pcbnew.B_Cu)
        need=0.3+0.2
        for px,py,pr in pf+pb:
            if M.hypot(vx-px,vy-py)<need+pr: return False
        for s in sf+sb:
            if spd(s[0],s[1],s[2],s[3],vx,vy)<need+s[4]: return False
        return True
    def find_via(px,py,tx,ty):
        for d in (0.9,1.3,1.7,2.2,2.8):
            L2=M.hypot(tx-px,ty-py); f=min(1,d/L2) if L2 else 0
            bx,by=px+(tx-px)*f,py+(ty-py)*f
            for off in range(0,360,30):
                for rr in (0,0.6,1.1):
                    vx=round(bx+rr*M.cos(M.radians(off)),3); vy=round(by+rr*M.sin(M.radians(off)),3)
                    if via_clear(vx,vy) and clean(b,px,py,vx,vy,net,pcbnew.F_Cu,w):
                        return (vx,vy)
        return None
    v0=find_via(x0,y0,x1,y1); v1=find_via(x1,y1,x0,y0)
    if v0 and v1:
        if clean(b,v0[0],v0[1],v1[0],v1[1],net,pcbnew.B_Cu,w):
            track(b,x0,y0,v0[0],v0[1],net,w,pcbnew.F_Cu);via(b,v0[0],v0[1],net)
            track(b,v0[0],v0[1],v1[0],v1[1],net,w,pcbnew.B_Cu)
            via(b,v1[0],v1[1],net);track(b,v1[0],v1[1],x1,y1,net,w,pcbnew.F_Cu);return 'directB'
        for cx,cy in [(v1[0],v0[1]),(v0[0],v1[1])]:
            if clean(b,v0[0],v0[1],cx,cy,net,pcbnew.B_Cu,w) and clean(b,cx,cy,v1[0],v1[1],net,pcbnew.B_Cu,w):
                track(b,x0,y0,v0[0],v0[1],net,w,pcbnew.F_Cu);via(b,v0[0],v0[1],net)
                track(b,v0[0],v0[1],cx,cy,net,w,pcbnew.B_Cu);track(b,cx,cy,v1[0],v1[1],net,w,pcbnew.B_Cu)
                via(b,v1[0],v1[1],net);track(b,v1[0],v1[1],x1,y1,net,w,pcbnew.F_Cu);return 'LB'
    return None
def astar(b,x0,y0,x1,y1,net,w,step=0.5,bounds=None):
    import heapq
    if bounds is None: bounds=(min(x0,x1)-15,max(x0,x1)+15,min(y0,y1)-15,max(y0,y1)+15)
    bx0,bx1,by0,by1=bounds
    # precompute obstacle checkers per layer via clean() on short segments
    def ok(ax,ay,bxx,byy,layer): return clean(b,ax,ay,bxx,byy,net,layer,w)
    def snap(x): return round(x/step)*step
    start=(snap(x0),snap(y0),0);goal=(snap(x1),snap(y1))
    import math as M
    def h(n): return M.hypot(n[0]-goal[0],n[1]-goal[1])
    pq=[(h(start),0,start,None)];came={};g={(start[0],start[1],start[2]):0};seen=set()
    dirs=[(step,0),(-step,0),(0,step),(0,-step),(step,step),(step,-step),(-step,step),(-step,-step)]
    cnt=0
    while pq and cnt<40000:
        cnt+=1
        f,gc,node,par=heapq.heappop(pq)
        key=(round(node[0],2),round(node[1],2),node[2])
        if key in seen: continue
        seen.add(key);came[key]=par
        if node[2]==0 and M.hypot(node[0]-goal[0],node[1]-goal[1])<=step*1.1 and ok(node[0],node[1],x1,y1,pcbnew.F_Cu):
            # reconstruct
            path=[(x1,y1,node[2])];k=key
            while k is not None:
                path.append((k[0],k[1],k[2]));k=came[k]
            path.reverse()
            return path
        layer=node[2]
        for dx,dy in dirs:
            nx,ny=round(node[0]+dx,2),round(node[1]+dy,2)
            if not(bx0<=nx<=bx1 and by0<=ny<=by1):continue
            pl=pcbnew.F_Cu if layer==0 else pcbnew.B_Cu
            if not ok(node[0],node[1],nx,ny,pl):continue
            nk=(nx,ny,layer);ng=gc+M.hypot(dx,dy)
            if nk not in g or ng<g[nk]:
                g[nk]=ng;heapq.heappush(pq,(ng+M.hypot(nx-goal[0],ny-goal[1]),ng,(nx,ny,layer),key))
        # layer switch (via) - allow at node
        ol=1-layer;pl=pcbnew.F_Cu if ol==0 else pcbnew.B_Cu
        nk=(round(node[0],2),round(node[1],2),ol);ng=gc+2.0
        if nk not in g or ng<g[nk]:
            g[nk]=ng;heapq.heappush(pq,(ng+h((node[0],node[1],ol)),ng,(node[0],node[1],ol),key))
    return None
def emit_path(b,path,net,w):
    # path: list of (x,y,layer 0/1). Add tracks + vias on layer changes.
    for i in range(len(path)-1):
        x0,y0,l0=path[i];x1,y1,l1=path[i+1]
        if l0!=l1:
            via(b,x0,y0,net)
        else:
            if (x0,y0)!=(x1,y1):
                track(b,x0,y0,x1,y1,net,w,pcbnew.F_Cu if l0==0 else pcbnew.B_Cu)
