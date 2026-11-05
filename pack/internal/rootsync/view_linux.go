package rootsync

import (
	"context"
	"fmt"
	"io"
	"math"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"syscall"

	"github.com/hanwen/go-fuse/v2/fs"
	"github.com/hanwen/go-fuse/v2/fuse"
	"golang.org/x/sys/unix"
)

// This is an I/O view, not a filesystem implementation: upstream mkfs/check
// tools own the on-disk format. The output file aliases only the GPT region;
// input files alias manifest sources without copying or changing their metadata.
type imageView struct {
	server *fuse.Server
	dir    string
}

func partitionBounds(disk *os.File, region PartitionRegion) (int64, int64, error) {
	if region.LastLBA < region.FirstLBA || region.LastLBA >= math.MaxInt64/sectorBytes {
		return 0, 0, fmt.Errorf("invalid partition bounds")
	}
	offset := int64(region.FirstLBA * sectorBytes)
	size := int64((region.LastLBA - region.FirstLBA + 1) * sectorBytes)
	info, err := disk.Stat()
	if err != nil {
		return 0, 0, err
	}
	if !info.Mode().IsRegular() || offset > info.Size() || size > info.Size()-offset {
		return 0, 0, fmt.Errorf("partition exceeds the configured regular disk image")
	}
	return offset, size, nil
}

func newImageView(disk *os.File, region PartitionRegion, manifest *Manifest, writable bool) (*imageView, error) {
	offset, size, err := partitionBounds(disk, region)
	if err != nil {
		return nil, err
	}
	input, err := manifestView(manifest)
	if err != nil {
		return nil, err
	}
	dir, err := os.MkdirTemp(filepath.Dir(disk.Name()), ".pacgo-view-*")
	if err != nil {
		return nil, err
	}
	root := &viewRoot{partition: &partitionView{disk: disk, offset: offset, size: size, writable: writable}, input: input}
	options := &fs.Options{NullPermissions: true, MountOptions: fuse.MountOptions{
		FsName: "pacgo-image-view", Name: "pacgo-view", DisableXAttrs: true}}
	if !writable {
		options.Options = []string{"ro"}
	}
	server, err := fs.Mount(dir, root, options)
	if err != nil {
		_ = os.Remove(dir)
		return nil, fmt.Errorf("rootless image view requires accessible /dev/fuse and fusermount3: %w", err)
	}
	return &imageView{server, dir}, nil
}

func (v *imageView) close() error {
	if err := v.server.Unmount(); err != nil {
		// Never recurse into a mount after failed unmount: its output is the
		// actual disk, and its input references the user's original artifacts.
		return fmt.Errorf("unmount %s (retained for cleanup): %w", v.dir, err)
	}
	v.server.Wait()
	return os.Remove(v.dir)
}

type viewRoot struct {
	fs.Inode
	partition *partitionView
	input     *inputView
}

func (r *viewRoot) OnAdd(ctx context.Context) {
	r.AddChild("partition", r.NewPersistentInode(ctx, r.partition, fs.StableAttr{Mode: syscall.S_IFREG}), false)
	if r.input != nil {
		r.AddChild("input", r.NewPersistentInode(ctx, r.input, fs.StableAttr{Mode: syscall.S_IFDIR}), false)
	}
}

type partitionView struct {
	fs.Inode
	disk         *os.File // Borrowed until the server is unmounted and drained.
	offset, size int64
	writable     bool
}

func (p *partitionView) Getattr(ctx context.Context, f fs.FileHandle, out *fuse.AttrOut) syscall.Errno {
	out.Mode = syscall.S_IFREG | 0600
	out.Size = uint64(p.size)
	out.Owner = fuse.Owner{Uid: uint32(os.Getuid()), Gid: uint32(os.Getgid())}
	return 0
}
func (p *partitionView) Open(ctx context.Context, flags uint32) (fs.FileHandle, uint32, syscall.Errno) {
	if flags&(syscall.O_WRONLY|syscall.O_RDWR|syscall.O_TRUNC) != 0 && !p.writable {
		return nil, 0, syscall.EROFS
	}
	// Metadata checks run immediately after mkfs; never retain a second,
	// incoherent kernel page cache for this alias of the underlying image.
	return nil, fuse.FOPEN_DIRECT_IO, 0
}
func (p *partitionView) Read(ctx context.Context, f fs.FileHandle, data []byte, off int64) (fuse.ReadResult, syscall.Errno) {
	if off < 0 || off > p.size {
		return nil, syscall.EINVAL
	}
	if int64(len(data)) > p.size-off {
		data = data[:p.size-off]
	}
	n, err := p.disk.ReadAt(data, p.offset+off)
	if err != nil && err != io.EOF {
		return nil, fs.ToErrno(err)
	}
	return fuse.ReadResultData(data[:n]), 0
}
func (p *partitionView) Write(ctx context.Context, f fs.FileHandle, data []byte, off int64) (uint32, syscall.Errno) {
	if !p.writable {
		return 0, syscall.EROFS
	}
	// Validate the entire request before writing anything, including overflow.
	// A malformed mkfs request must never reach the ESP or either GPT copy.
	if off < 0 || off > p.size || int64(len(data)) > p.size-off {
		return 0, syscall.EFBIG
	}
	n, err := p.disk.WriteAt(data, p.offset+off)
	return uint32(n), fs.ToErrno(err)
}
func (p *partitionView) Setattr(ctx context.Context, f fs.FileHandle, in *fuse.SetAttrIn, out *fuse.AttrOut) syscall.Errno {
	if !p.writable {
		return syscall.EROFS
	}
	if size, ok := in.GetSize(); ok && size != uint64(p.size) {
		return syscall.EFBIG
	}
	return p.Getattr(ctx, f, out)
}
func (p *partitionView) Fsync(ctx context.Context, f fs.FileHandle, flags uint32) syscall.Errno {
	return fs.ToErrno(p.disk.Sync())
}

type inputView struct {
	fs.Inode
	attr     fuse.Attr
	source   string
	target   []byte
	children map[string]*inputView
}

func manifestView(manifest *Manifest) (*inputView, error) {
	if manifest == nil {
		return nil, nil
	}
	root := &inputView{attr: fuse.Attr{Mode: syscall.S_IFDIR | 0755}, children: map[string]*inputView{}}
	root.attr.Owner = inputOwner()
	for _, dir := range manifest.Dirs {
		if _, err := root.directory(dir.Path); err != nil {
			return nil, err
		}
	}
	for _, spec := range manifest.Files {
		if err := validateImagePath(spec.ImagePath); err != nil {
			return nil, err
		}
		parent, err := root.directory(path.Dir(spec.ImagePath))
		if err != nil {
			return nil, err
		}
		name := path.Base(spec.ImagePath)
		if parent.children[name] != nil {
			return nil, fmt.Errorf("duplicate manifest path %s", spec.ImagePath)
		}
		node := &inputView{attr: fuse.Attr{Mode: syscall.S_IFREG | 0644, Size: spec.Size, Nlink: 1}, source: spec.SourcePath}
		node.attr.Owner = inputOwner()
		switch {
		case spec.IsSymlink:
			node.attr.Mode = syscall.S_IFLNK | 0777
			node.target = spec.SymlinkTarget
			node.attr.Size = uint64(len(node.target))
		case spec.IsDeviceNode:
			if (spec.DeviceType != 'c' && spec.DeviceType != 'b') || spec.DeviceMajor > 4095 || spec.DeviceMinor > 1048575 {
				return nil, fmt.Errorf("invalid device number: %s", spec.ImagePath)
			}
			node.attr.Mode = syscall.S_IFCHR | 0600
			if spec.DeviceType == 'b' {
				node.attr.Mode = syscall.S_IFBLK | 0600
			}
			node.attr.Rdev = uint32(unix.Mkdev(spec.DeviceMajor, spec.DeviceMinor))
			node.attr.Size = 0
		default:
			info, err := os.Stat(spec.SourcePath)
			if err != nil {
				return nil, err
			}
			if !info.Mode().IsRegular() || info.Size() < 0 || uint64(info.Size()) != spec.Size {
				return nil, fmt.Errorf("manifest source changed: %s", spec.SourcePath)
			}
			node.attr.Mode = syscall.S_IFREG | uint32(info.Mode().Perm())
			if info.Mode()&os.ModeSetuid != 0 {
				node.attr.Mode |= syscall.S_ISUID
			}
			if info.Mode()&os.ModeSetgid != 0 {
				node.attr.Mode |= syscall.S_ISGID
			}
			if info.Mode()&os.ModeSticky != 0 {
				node.attr.Mode |= syscall.S_ISVTX
			}
		}
		parent.children[name] = node
	}
	return root, nil
}

func (n *inputView) directory(name string) (*inputView, error) {
	if name == "/" {
		return n, nil
	}
	if err := validateImagePath(name); err != nil {
		return nil, err
	}
	for _, component := range strings.Split(strings.TrimPrefix(name, "/"), "/") {
		child := n.children[component]
		if child == nil {
			child = &inputView{attr: fuse.Attr{Mode: syscall.S_IFDIR | 0755}, children: map[string]*inputView{}}
			child.attr.Owner = inputOwner()
			n.children[component] = child
		}
		if child.children == nil {
			return nil, fmt.Errorf("manifest directory conflicts with file: %s", name)
		}
		n = child
	}
	return n, nil
}

func inputOwner() fuse.Owner {
	// mkfs runs in an unprivileged user namespace mapping this host user to
	// guest uid/gid 0. Keeping the real host owner here also permits O_NOATIME
	// source opens without CAP_FOWNER in the host's initial user namespace.
	return fuse.Owner{Uid: uint32(os.Getuid()), Gid: uint32(os.Getgid())}
}
func (n *inputView) OnAdd(ctx context.Context) {
	names := make([]string, 0, len(n.children))
	for name := range n.children {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		child := n.children[name]
		n.AddChild(name, n.NewPersistentInode(ctx, child, fs.StableAttr{Mode: child.attr.Mode & syscall.S_IFMT}), false)
	}
}
func (n *inputView) Getattr(ctx context.Context, f fs.FileHandle, out *fuse.AttrOut) syscall.Errno {
	out.Attr = n.attr
	return 0
}
func (n *inputView) Access(ctx context.Context, mask uint32) syscall.Errno {
	// Tools must read guest-root-owned inputs even when they run as the host
	// user. This private mount has no allow_other and never permits mutations.
	if mask&2 != 0 {
		return syscall.EROFS
	}
	return 0
}
func (n *inputView) Readlink(ctx context.Context) ([]byte, syscall.Errno) { return n.target, 0 }
func (n *inputView) Open(ctx context.Context, flags uint32) (fs.FileHandle, uint32, syscall.Errno) {
	if flags&(syscall.O_WRONLY|syscall.O_RDWR|syscall.O_TRUNC) != 0 {
		return nil, 0, syscall.EROFS
	}
	f, err := os.Open(n.source)
	if err != nil {
		return nil, 0, fs.ToErrno(err)
	}
	return &inputHandle{f}, fuse.FOPEN_KEEP_CACHE, 0
}

type inputHandle struct{ file *os.File }

func (h *inputHandle) Read(ctx context.Context, data []byte, off int64) (fuse.ReadResult, syscall.Errno) {
	return fuse.ReadResultFd(h.file.Fd(), off, len(data)), 0
}
func (h *inputHandle) Lseek(ctx context.Context, off uint64, whence uint32) (uint64, syscall.Errno) {
	if off > math.MaxInt64 {
		return 0, syscall.EINVAL
	}
	pos, err := unix.Seek(int(h.file.Fd()), int64(off), int(whence))
	if err != nil {
		return 0, fs.ToErrno(err)
	}
	return uint64(pos), 0
}
func (h *inputHandle) Release(ctx context.Context) syscall.Errno { return fs.ToErrno(h.file.Close()) }
