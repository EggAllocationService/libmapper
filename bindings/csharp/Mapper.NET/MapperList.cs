using System.Collections;
using System.Runtime.InteropServices;

namespace Mapper;

public abstract class _MapperList
{
    private IntPtr _list;
    private bool _started;
    protected MapperType MapperType;

    public _MapperList()
    {
        MapperType = MapperType.Null;
        _list = IntPtr.Zero;
        _started = false;
    }

    public _MapperList(IntPtr list, MapperType mapperType)
    {
        MapperType = mapperType;
        _list = list;
        _started = false;
    }

    /* copy constructor */
    public _MapperList(_MapperList original)
    {
        _list = mpr_list_get_cpy(original._list);
        MapperType = original.MapperType;
        _started = false;
    }

    public int Count => mpr_list_get_size(_list);

    // TODO: probably need refcounting to check if we should free the underlying mpr_list
    [DllImport("mapper", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.StdCall)]
    private static extern void mpr_list_free(IntPtr list);

    public void Free()
    {
        mpr_list_free(_list);
        _list = IntPtr.Zero;
    }

    [DllImport("mapper", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.StdCall)]
    private static extern int mpr_list_get_size(IntPtr list);

    [DllImport("mapper", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.StdCall)]
    private static extern IntPtr mpr_list_get_idx(IntPtr list, int index);

    public IntPtr GetIdx(int index)
    {
        return mpr_list_get_idx(_list, index);
    }

    public IntPtr Deref()
    {
        unsafe
        {
            return new IntPtr(_list != IntPtr.Zero ? *(void**)_list : null);
        }
    }

    [DllImport("mapper", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.StdCall)]
    private static extern IntPtr mpr_list_get_cpy(IntPtr list);

    [DllImport("mapper", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.StdCall)]
    private static extern IntPtr mpr_list_get_union(IntPtr list1, IntPtr list2);

    public _MapperList Join(_MapperList rhs)
    {
        _list = mpr_list_get_union(_list, mpr_list_get_cpy(rhs._list));
        return this;
    }

    public static IntPtr Union(_MapperList a, _MapperList b)
    {
        return mpr_list_get_union(mpr_list_get_cpy(a._list), mpr_list_get_cpy(b._list));
    }

    [DllImport("mapper", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.StdCall)]
    private static extern IntPtr mpr_list_get_isect(IntPtr list1, IntPtr list2);

    public _MapperList Intersect(_MapperList rhs)
    {
        _list = mpr_list_get_isect(_list, mpr_list_get_cpy(rhs._list));
        return this;
    }

    public static IntPtr Intersection(_MapperList a, _MapperList b)
    {
        return mpr_list_get_isect(mpr_list_get_cpy(a._list), mpr_list_get_cpy(b._list));
    }

    [DllImport("mapper", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.StdCall)]
    private static extern IntPtr mpr_list_get_diff(IntPtr list1, IntPtr list2);

    public _MapperList Subtract(_MapperList rhs)
    {
        _list = mpr_list_get_diff(_list, mpr_list_get_cpy(rhs._list));
        return this;
    }

    public static IntPtr Difference(_MapperList a, _MapperList b)
    {
        return mpr_list_get_diff(mpr_list_get_cpy(a._list), mpr_list_get_cpy(b._list));
    }

    [DllImport("mapper", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.StdCall)]
    private static extern IntPtr mpr_list_get_next(IntPtr list);

    public bool GetNext()
    {
        if (_started)
            _list = mpr_list_get_next(_list);
        else
            _started = true;
        return _list != IntPtr.Zero;
    }

    public override string ToString()
    {
        return $"Mapper.List<{MapperType}>";
    }
}

public class MapperList<T> : _MapperList, IEnumerator, IEnumerable, IDisposable
    where T : MapperObject, new()
{
    internal MapperList(IntPtr list, MapperType mapperType) : base(list, mapperType)
    {
    }

    public T this[int index]
    {
        get
        {
            var t = new T();
            t.NativePtr = GetIdx(index);
            return t;
        }
    }

    public T Current
    {
        get
        {
            var t = new T();
            t.NativePtr = Deref();
            return t;
        }
    }

    void IDisposable.Dispose()
    {
        Free();
    }

    /* Methods for enumeration */
    public IEnumerator GetEnumerator()
    {
        return this;
    }

    public void Reset()
    {
        throw new NotSupportedException();
    }

    public bool MoveNext()
    {
        return GetNext();
    }

    object IEnumerator.Current => Current;

    /* Overload some arithmetic operators */
    public static MapperList<T> operator +(MapperList<T> a, MapperList<T> b)
    {
        return new MapperList<T>(Union(a, b), a.MapperType);
    }

    public static MapperList<T> operator *(MapperList<T> a, MapperList<T> b)
    {
        return new MapperList<T>(Intersection(a, b), a.MapperType);
    }

    public static MapperList<T> operator -(MapperList<T> a, MapperList<T> b)
    {
        return new MapperList<T>(Difference(a, b), a.MapperType);
    }
}