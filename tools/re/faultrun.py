"""Run a fault-injection sweep across N git worktrees in parallel.

The serial version patched one file in the working tree, rebuilt, ran the whole
suite, and restored -- about 25 seconds a fault, and a single-file rebuild gives
ninja nothing to parallelise, so twenty faults was ten minutes of one core.

This gives each worker its own worktree and its own build directory, so N faults
compile and run at once. The worktrees persist for the session: the first sweep
pays for N full builds (in parallel), every sweep after it pays nothing.

    from faultrun import sweep
    sweep(FILES, FAULTS)

`FILES` maps a short key to a repo-relative path. `FAULTS` maps a description to
`(key, old, new)`, exactly as the serial driver took them -- the sweep scripts
need no other change.

**This is a background tool and it must leave the machine usable.** Two things
keep it there, and both were once wrong:

  * every worker's `ninja` runs at **`-j1`**. A fault patches one file, so the
    rebuild has nothing to parallelise anyway -- but ninja's default is
    `cpus + 2`, and N workers each defaulting to that is `N * (cpus + 2)`
    compilers on a machine with `cpus` of them. Six workers on a ten-core
    machine was seventy-two.
  * the worker count leaves **at least two cores free** and is capped, so the
    sweep's own total is `workers` compilers plus one test binary.

`IMPERIVM_FAULT_WORKERS` overrides the count for a machine that wants
otherwise. Nothing here should ever be run without a `-j` bound.
"""
import concurrent.futures, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
POOL = os.environ.get('IMPERIVM_FAULTPOOL', '/tmp/imperivm-faultpool')


def default_workers():
    """As many parallel faults as fit while leaving two cores for everything
    else, capped at six -- past that the test binaries contend for memory
    bandwidth and the wall clock stops improving."""
    override = os.environ.get('IMPERIVM_FAULT_WORKERS')
    if override:
        return max(1, int(override))
    return max(1, min(6, (os.cpu_count() or 4) - 2))


def _ninja(build_dir, jobs=1):
    """Never without a `-j`. See the header."""
    return _run(['ninja', '-j%d' % max(1, jobs), '-C', build_dir])


def _run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def _worktree(i):
    return os.path.join(POOL, 'wt%d' % i)


def ensure_pool(workers):
    """Create and warm `workers` worktrees. Idempotent, and the slow part is
    only paid the first time in a session."""
    os.makedirs(POOL, exist_ok=True)
    # A worktree has to be at the same commit as the tree being tested, and the
    # working tree is usually dirty -- so the sources are copied in below rather
    # than checked out. `git worktree` is only used for a valid repo skeleton.
    made = []
    for i in range(workers):
        wt = _worktree(i)
        if not os.path.isdir(os.path.join(wt, 'engine')):
            _run(['git', 'worktree', 'add', '--detach', wt, 'HEAD'], cwd=ROOT)
            made.append(i)
    def warm(i):
        wt = _worktree(i)
        sync(wt)
        b = _run(['cmake', '-S', wt, '-B', os.path.join(wt, 'build-core'), '-G', 'Ninja',
                  '-DIMPERIVM_BUILD_APP=OFF'])
        if b.returncode != 0:
            return 'configure failed: ' + b.stderr[-400:]
        # The warm build is a full one, so the whole budget is split between
        # the workers rather than handed to each of them.
        b = _ninja(os.path.join(wt, 'build-core'),
                   max(1, ((os.cpu_count() or 4) - 2) // max(1, workers)))
        return None if b.returncode == 0 else 'build failed: ' + b.stderr[-400:]
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for err in pool.map(warm, range(workers)):
            if err:
                print('WARM:', err, file=sys.stderr)
    return made


def sync(wt):
    """Copy the live sources into a worktree. Only the trees a sweep can touch,
    so this stays a fraction of a second."""
    for sub in ('engine',):
        subprocess.run(['rsync', '-a', '--delete',
                        os.path.join(ROOT, sub) + '/', os.path.join(wt, sub) + '/'],
                       capture_output=True)


def sweep(files, faults, workers=None, verbose=True):
    """`files`: {key: repo-relative path}. `faults`: {name: (key, old, new)}."""
    workers = default_workers() if workers is None else max(1, min(workers, default_workers()))
    ensure_pool(workers)
    base = {k: open(os.path.join(ROOT, p)).read() for k, p in files.items()}
    # Every worktree starts from the live sources.
    for i in range(workers):
        sync(_worktree(i))

    items = list(faults.items())
    results = [None] * len(items)
    slots = list(range(workers))
    lock = __import__('threading').Lock()

    def take():
        with lock:
            return slots.pop()

    def give(i):
        with lock:
            slots.append(i)

    def one(job):
        index, (name, (key, old, new)) = job
        src = base[key]
        if src.count(old) != 1:
            return index, (name, 'PATCH FAILED count=%d' % src.count(old))
        slot = take()
        wt = _worktree(slot)
        path = os.path.join(wt, files[key])
        try:
            open(path, 'w').write(src.replace(old, new, 1))
            b = _ninja(os.path.join(wt, 'build-core'))
            if b.returncode != 0:
                return index, (name, 'BUILD FAILED')
            try:
                r = _run([os.path.join(wt, 'build-core/engine/tests/core_tests')], timeout=300)
            except subprocess.TimeoutExpired:
                return index, (name, 'TIMEOUT (caught)')
            tail = r.stdout.strip().split('\n')[-1]
            fails = sorted({l.strip() for l in r.stdout.split('\n') if l.startswith('FAIL ')})
            note = ('  <-- SURVIVED' if ' 0 failures' in tail
                    else '  caught by: ' + ', '.join(f[5:] for f in fails[:2]))
            return index, (name, tail + note)
        finally:
            open(path, 'w').write(src)
            give(slot)

    started = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for index, row in pool.map(one, list(enumerate(items))):
            results[index] = row
            if verbose:
                print('%-66s %s' % row, flush=True)
    # Leave every worktree matching the live sources, so the next sweep's
    # incremental build is one file rather than all of them.
    for i in range(workers):
        b = _ninja(os.path.join(_worktree(i), 'build-core'))
        del b
    if verbose:
        print('\n%d faults in %.0fs across %d worktrees' % (len(items), time.time() - started, workers))
    return results
