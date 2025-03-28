// SPDX-License-Identifier: GPL-2.0

//! Device Tree / Open Firmware abstractions.

use crate::{
    bindings, device_id::RawDeviceId, error::to_result, io::resource::Resource, prelude::*,
};
// Note: Most OF functions turn into inline dummies with CONFIG_OF(_*) disabled.
// We have to either add config conditionals to helpers.c or here; let's do it
// here for now. In the future, once bindgen can auto-generate static inline
// helpers, this can go away if desired.

use core::marker::PhantomData;
use core::num::NonZeroU32;

/// IdTable type for OF drivers.
pub type IdTable<T> = &'static dyn kernel::device_id::IdTable<DeviceId, T>;

/// An open firmware device id.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct DeviceId(bindings::of_device_id);

// SAFETY:
// * `DeviceId` is a `#[repr(transparent)` wrapper of `struct of_device_id` and does not add
//   additional invariants, so it's safe to transmute to `RawType`.
// * `DRIVER_DATA_OFFSET` is the offset to the `data` field.
unsafe impl RawDeviceId for DeviceId {
    type RawType = bindings::of_device_id;

    const DRIVER_DATA_OFFSET: usize = core::mem::offset_of!(bindings::of_device_id, data);

    fn index(&self) -> usize {
        self.0.data as _
    }
}

impl DeviceId {
    /// Create a new device id from an OF 'compatible' string.
    pub const fn new(compatible: &'static CStr) -> Self {
        let src = compatible.as_bytes_with_nul();
        // Replace with `bindings::of_device_id::default()` once stabilized for `const`.
        // SAFETY: FFI type is valid to be zero-initialized.
        let mut of: bindings::of_device_id = unsafe { core::mem::zeroed() };

        // TODO: Use `clone_from_slice` once the corresponding types do match.
        let mut i = 0;
        while i < src.len() {
            of.compatible[i] = src[i] as _;
            i += 1;
        }

        Self(of)
    }
}

/// Type alias for an OF phandle
pub type PHandle = bindings::phandle;

/// An OF device tree node.
///
/// # Invariants
///
/// `raw_node` points to a valid OF node, and we hold a reference to it.
pub struct Node {
    raw_node: *mut bindings::device_node,
}

#[allow(dead_code)]
impl Node {
    /// Creates a `Node` from a raw C pointer. The pointer must be owned (the caller
    /// gives up its reference). If the pointer is NULL, returns None.
    pub(crate) unsafe fn from_raw(raw_node: *mut bindings::device_node) -> Option<Node> {
        if raw_node.is_null() {
            None
        } else {
            // INVARIANT: `raw_node` is valid per the above contract, and non-null per the
            // above check.
            Some(Node { raw_node })
        }
    }

    /// Creates a `Node` from a raw C pointer. The pointer must be borrowed (the caller
    /// retains its reference, which must be valid for the duration of the call). If the
    /// pointer is NULL, returns None.
    pub(crate) unsafe fn get_from_raw(raw_node: *mut bindings::device_node) -> Option<Node> {
        // SAFETY: `raw_node` is valid or NULL per the above contract. `of_node_get` can handle
        // NULL.
        unsafe {
            #[cfg(CONFIG_OF_DYNAMIC)]
            bindings::of_node_get(raw_node);
            Node::from_raw(raw_node)
        }
    }

    /// Returns a reference to the underlying C `device_node` structure.
    pub(crate) fn node(&self) -> &bindings::device_node {
        // SAFETY: `raw_node` is valid per the type invariant.
        unsafe { &*self.raw_node }
    }

    /// Returns a reference to the underlying C `device_node` structure.
    pub unsafe fn as_raw(&self) -> *mut bindings::device_node {
        self.raw_node
    }

    /// Returns the name of the node.
    pub fn name(&self) -> &CStr {
        // SAFETY: The lifetime of the `CStr` is the same as the lifetime of this `Node`.
        unsafe { CStr::from_char_ptr(self.node().name) }
    }

    /// Returns the phandle for this node.
    pub fn phandle(&self) -> PHandle {
        self.node().phandle
    }

    /// Returns the full name (with address) for this node.
    pub fn full_name(&self) -> &CStr {
        // SAFETY: The lifetime of the `CStr` is the same as the lifetime of this `Node`.
        unsafe { CStr::from_char_ptr(self.node().full_name) }
    }

    /// Returns `true` if the node is the root node.
    pub fn is_root(&self) -> bool {
        #[cfg(not(CONFIG_OF))]
        {
            false
        }
        #[cfg(CONFIG_OF)]
        // SAFETY: `raw_node` is valid per the type invariant
        unsafe {
            bindings::of_node_is_root(self.raw_node)
        }
    }

    /// Returns the parent node, if any.
    pub fn parent(&self) -> Option<Node> {
        #[cfg(not(CONFIG_OF))]
        {
            None
        }
        #[cfg(CONFIG_OF)]
        // SAFETY: `raw_node` is valid per the type invariant, and `of_get_parent()` takes a
        // new reference to the parent (or returns NULL).
        unsafe {
            Node::from_raw(bindings::of_get_parent(self.raw_node))
        }
    }

    /// Returns an iterator over the node's children.
    // TODO: use type alias for return type once type_alias_impl_trait is stable
    pub fn children(
        &self,
    ) -> NodeIterator<'_, impl Fn(*mut bindings::device_node) -> *mut bindings::device_node + '_>
    {
        #[cfg(not(CONFIG_OF))]
        {
            NodeIterator::new(|_prev| core::ptr::null_mut())
        }
        #[cfg(CONFIG_OF)]
        // SAFETY: `raw_node` is valid per the type invariant, and the lifetime of the `NodeIterator`
        // does not exceed the lifetime of the `Node` so it can borrow its reference.
        NodeIterator::new(|prev| unsafe { bindings::of_get_next_child(self.raw_node, prev) })
    }

    /// Find a child by its name and return it, or None if not found.
    #[allow(unused_variables)]
    pub fn get_child_by_name(&self, name: &CStr) -> Option<Node> {
        #[cfg(not(CONFIG_OF))]
        {
            None
        }
        #[cfg(CONFIG_OF)]
        // SAFETY: `raw_node` is valid per the type invariant.
        unsafe {
            Node::from_raw(bindings::of_get_child_by_name(
                self.raw_node,
                name.as_char_ptr(),
            ))
        }
    }

    /// Checks whether the node is compatible with the given compatible string.
    ///
    /// Returns `None` if there is no match, or `Some<NonZeroU32>` if there is, with the value
    /// representing as match score (higher values for more specific compatible matches).
    #[allow(unused_variables)]
    pub fn is_compatible(&self, compatible: &CStr) -> Option<NonZeroU32> {
        #[cfg(not(CONFIG_OF))]
        let ret = 0;
        #[cfg(CONFIG_OF)]
        let ret =
            // SAFETY: `raw_node` is valid per the type invariant.
            unsafe { bindings::of_device_is_compatible(self.raw_node, compatible.as_char_ptr()) };

        NonZeroU32::new(ret.try_into().ok()?)
    }

    /// Parse a phandle property and return the Node referenced at a given index, if any.
    ///
    /// Used only for phandle properties with no arguments.
    #[allow(unused_variables)]
    pub fn parse_phandle(&self, name: &CStr, index: usize) -> Option<Node> {
        #[cfg(not(CONFIG_OF))]
        {
            None
        }
        #[cfg(CONFIG_OF)]
        // SAFETY: `raw_node` is valid per the type invariant. `of_parse_phandle` returns an
        // owned reference.
        unsafe {
            Node::from_raw(bindings::of_parse_phandle(
                self.raw_node,
                name.as_char_ptr(),
                index.try_into().ok()?,
            ))
        }
    }

    /// Parse a phandle property and return the Node referenced at a given name, if any.
    ///
    /// Used only for phandle properties with no arguments.
    #[allow(unused_variables)]
    pub fn parse_phandle_by_name(
        &self,
        prop: &CStr,
        propnames: &CStr,
        name: &CStr,
    ) -> Option<Node> {
        #[cfg(not(CONFIG_OF))]
        {
            None
        }
        #[cfg(CONFIG_OF)]
        // SAFETY: `raw_node` is valid per the type invariant. `of_parse_phandle` returns an
        // owned reference.
        unsafe {
            let index = bindings::of_property_match_string(
                self.raw_node,
                propnames.as_char_ptr(),
                name.as_char_ptr(),
            );
            if index < 0 {
                return None;
            };

            Node::from_raw(bindings::of_parse_phandle(
                self.raw_node,
                prop.as_char_ptr(),
                index.try_into().ok()?,
            ))
        }
    }

    /// Translate device tree address and return as resource
    pub fn address_as_resource(&self, index: usize) -> Result<Resource> {
        #[cfg(not(CONFIG_OF))]
        {
            Err(EINVAL)
        }
        #[cfg(CONFIG_OF)]
        {
            let mut res = core::mem::MaybeUninit::<bindings::resource>::uninit();
            // SAFETY: This function is safe to call as long as the arguments are valid pointers.
            let ret = unsafe {
                bindings::of_address_to_resource(self.raw_node, index.try_into()?, res.as_mut_ptr())
            };
            to_result(ret)?;
            // SAFETY: We have checked the return value above, so the resource must be initialized now
            let res = unsafe { res.assume_init() };

            Ok(Resource::new_from_ptr(&res))
        }
    }

    #[allow(unused_variables)]
    /// Check whether node property exists.
    pub fn property_present(&self, propname: &CStr) -> bool {
        #[cfg(not(CONFIG_OF))]
        {
            false
        }
        #[cfg(CONFIG_OF)]
        // SAFETY: `raw_node` is valid per the type invariant.
        unsafe {
            bool::from(bindings::of_property_present(
                self.raw_node,
                propname.as_char_ptr(),
            ))
        }
    }

    #[allow(unused_variables)]
    /// Look up a node property by name, returning a `Property` object if found.
    pub fn find_property(&self, propname: &CStr) -> Option<Property<'_>> {
        #[cfg(not(CONFIG_OF))]
        {
            None
        }
        #[cfg(CONFIG_OF)]
        // SAFETY: `raw_node` is valid per the type invariant. The property structure
        // returned borrows the reference to the owning node, and so has the same
        // lifetime.
        unsafe {
            Property::from_raw(bindings::of_find_property(
                self.raw_node,
                propname.as_char_ptr(),
                core::ptr::null_mut(),
            ))
        }
    }

    /// Look up a mandatory node property by name, and decode it into a value type.
    ///
    /// Returns `Err(ENOENT)` if the property is not found.
    ///
    /// The type `T` must implement `TryFrom<Property<'_>>`.
    pub fn get_property<'a, T: TryFrom<Property<'a>>>(&'a self, propname: &CStr) -> Result<T>
    where
        crate::error::Error: From<<T as TryFrom<Property<'a>>>::Error>,
    {
        Ok(self.find_property(propname).ok_or(ENOENT)?.try_into()?)
    }

    /// Look up an optional node property by name, and decode it into a value type.
    ///
    /// Returns `Ok(None)` if the property is not found.
    ///
    /// The type `T` must implement `TryFrom<Property<'_>>`.
    pub fn get_opt_property<'a, T: TryFrom<Property<'a>>>(
        &'a self,
        propname: &CStr,
    ) -> Result<Option<T>>
    where
        crate::error::Error: From<<T as TryFrom<Property<'a>>>::Error>,
    {
        self.find_property(propname)
            .map_or(Ok(None), |p| Ok(Some(p.try_into()?)))
    }
}

/// A property attached to a device tree `Node`.
///
/// # Invariants
///
/// `raw` must be valid and point to a property that outlives the lifetime of this object.
#[derive(Copy, Clone)]
pub struct Property<'a> {
    raw: *mut bindings::property,
    _p: PhantomData<&'a Node>,
}

impl<'a> Property<'a> {
    #[cfg(CONFIG_OF)]
    /// Create a `Property` object from a raw C pointer. Returns `None` if NULL.
    ///
    /// The passed pointer must be valid and outlive the lifetime argument, or NULL.
    unsafe fn from_raw(raw: *mut bindings::property) -> Option<Property<'a>> {
        if raw.is_null() {
            None
        } else {
            Some(Property {
                raw,
                _p: PhantomData,
            })
        }
    }

    /// Returns the name of the property as a `CStr`.
    pub fn name(&self) -> &CStr {
        // SAFETY: `raw` is valid per the type invariant, and the lifetime of the `CStr` does not
        // outlive it.
        unsafe { CStr::from_char_ptr((*self.raw).name) }
    }

    /// Returns the name of the property as a `&[u8]`.
    pub fn value(&self) -> &[u8] {
        // SAFETY: `raw` is valid per the type invariant, and the lifetime of the slice does not
        // outlive it.
        unsafe { core::slice::from_raw_parts((*self.raw).value as *const u8, self.len()) }
    }

    /// Returns the length of the property in bytes.
    pub fn len(&self) -> usize {
        // SAFETY: `raw` is valid per the type invariant.
        unsafe { (*self.raw).length.try_into().unwrap() }
    }

    /// Returns true if the property is empty (zero-length), which typically represents boolean true.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Copy a device-tree property to a slice
    ///
    /// Enforces that the length of the property is an exact match of the slice.
    pub fn copy_to_slice<T: PropertyUnit>(&self, target: &mut [T]) -> Result<()> {
        if self.len() % T::UNIT_SIZE != 0 {
            return Err(EINVAL);
        }

        if self.len() / T::UNIT_SIZE != target.len() {
            return Err(EINVAL);
        }

        let val = self.value();
        for (i, off) in (0..self.len()).step_by(T::UNIT_SIZE).enumerate() {
            target[i] = T::from_bytes(&val[off..off + T::UNIT_SIZE])?
        }
        Ok(())
    }
}

/// A trait that represents a value decodable from a property with a fixed unit size.
///
/// This allows us to auto-derive property decode implementations for `Vec<T: PropertyUnit>`.
pub trait PropertyUnit: Sized {
    /// The size in bytes of a single data unit.
    const UNIT_SIZE: usize;

    /// Decode this data unit from a byte slice. The passed slice will have a length of `UNIT_SIZE`.
    fn from_bytes(data: &[u8]) -> Result<Self>;
}

// This doesn't work...
// impl<'a, T: PropertyUnit> TryFrom<Property<'a>> for T {
//     type Error = Error;
//
//     fn try_from(p: Property<'_>) -> core::result::Result<T, Self::Error> {
//         if p.value().len() != T::UNIT_SIZE {
//             Err(EINVAL)
//         } else {
//             Ok(T::from_bytes(p.value())?)
//         }
//     }
// }

impl<'a, T: PropertyUnit> TryFrom<Property<'a>> for KVec<T> {
    type Error = Error;

    fn try_from(p: Property<'_>) -> core::result::Result<KVec<T>, Self::Error> {
        if p.len() % T::UNIT_SIZE != 0 {
            return Err(EINVAL);
        }

        let mut v = Vec::new();
        let val = p.value();
        for off in (0..p.len()).step_by(T::UNIT_SIZE) {
            v.push(T::from_bytes(&val[off..off + T::UNIT_SIZE])?, GFP_KERNEL)?;
        }
        Ok(v)
    }
}

macro_rules! prop_int_type (
    ($type:ty) => {
        impl<'a> TryFrom<Property<'a>> for $type {
            type Error = Error;

            fn try_from(p: Property<'_>) -> core::result::Result<$type, Self::Error> {
                Ok(<$type>::from_be_bytes(p.value().try_into().or(Err(EINVAL))?))
            }
        }

        impl PropertyUnit for $type {
            const UNIT_SIZE: usize = <$type>::BITS as usize / 8;

            fn from_bytes(data: &[u8]) -> Result<Self> {
                Ok(<$type>::from_be_bytes(data.try_into().or(Err(EINVAL))?))
            }
        }
    }
);

prop_int_type!(u8);
prop_int_type!(u16);
prop_int_type!(u32);
prop_int_type!(u64);
prop_int_type!(i8);
prop_int_type!(i16);
prop_int_type!(i32);
prop_int_type!(i64);

/// An iterator across a collection of Node objects.
///
/// # Invariants
///
/// `cur` must be NULL or a valid node owned reference. If NULL, it represents either the first
/// or last position of the iterator.
///
/// If `done` is true, `cur` must be NULL.
///
/// fn_next must be a callback that iterates from one node to the next, and it must not capture
/// values that exceed the lifetime of the iterator. It must return owned references and also
/// take owned references.
pub struct NodeIterator<'a, T>
where
    T: Fn(*mut bindings::device_node) -> *mut bindings::device_node,
{
    cur: *mut bindings::device_node,
    done: bool,
    fn_next: T,
    _p: PhantomData<&'a T>,
}

impl<'a, T> NodeIterator<'a, T>
where
    T: Fn(*mut bindings::device_node) -> *mut bindings::device_node,
{
    fn new(next: T) -> NodeIterator<'a, T> {
        // INVARIANT: `cur` is initialized to NULL to represent the initial state.
        NodeIterator {
            cur: core::ptr::null_mut(),
            done: false,
            fn_next: next,
            _p: PhantomData,
        }
    }
}

impl<'a, T> Iterator for NodeIterator<'a, T>
where
    T: Fn(*mut bindings::device_node) -> *mut bindings::device_node,
{
    type Item = Node;

    fn next(&mut self) -> Option<Self::Item> {
        if self.done {
            None
        } else {
            // INVARIANT: if the new `cur` is NULL, then the iterator has reached its end and we
            // set `done` to `true`.
            self.cur = (self.fn_next)(self.cur);
            self.done = self.cur.is_null();
            // SAFETY: `fn_next` must return an owned reference per the iterator contract.
            // The iterator itself is considered to own this reference, so we take another one.
            unsafe { Node::get_from_raw(self.cur) }
        }
    }
}

// Drop impl to ensure we drop the current node being iterated on, if any.
impl<'a, T> Drop for NodeIterator<'a, T>
where
    T: Fn(*mut bindings::device_node) -> *mut bindings::device_node,
{
    fn drop(&mut self) {
        // SAFETY: `cur` is valid or NULL, and `of_node_put()` can handle NULL.
        #[cfg(CONFIG_OF_DYNAMIC)]
        unsafe {
            bindings::of_node_put(self.cur)
        };
    }
}

/// Returns the root node of the OF device tree (if any).
pub fn root() -> Option<Node> {
    #[cfg(not(CONFIG_OF))]
    {
        None
    }
    #[cfg(CONFIG_OF)]
    // SAFETY: bindings::of_root is always valid or NULL
    unsafe {
        Node::get_from_raw(bindings::of_root)
    }
}

/// Returns the /chosen node of the OF device tree (if any).
pub fn chosen() -> Option<Node> {
    #[cfg(not(CONFIG_OF))]
    {
        None
    }
    #[cfg(CONFIG_OF)]
    // SAFETY: bindings::of_chosen is always valid or NULL
    unsafe {
        Node::get_from_raw(bindings::of_chosen)
    }
}

/// Returns the /aliases node of the OF device tree (if any).
pub fn aliases() -> Option<Node> {
    #[cfg(not(CONFIG_OF))]
    {
        None
    }
    #[cfg(CONFIG_OF)]
    // SAFETY: bindings::of_aliases is always valid or NULL
    unsafe {
        Node::get_from_raw(bindings::of_aliases)
    }
}

/// Returns the system stdout node of the OF device tree (if any).
pub fn stdout() -> Option<Node> {
    #[cfg(not(CONFIG_OF))]
    {
        None
    }
    #[cfg(CONFIG_OF)]
    // SAFETY: bindings::of_stdout is always valid or NULL
    unsafe {
        Node::get_from_raw(bindings::of_stdout)
    }
}

#[allow(unused_variables)]
/// Looks up a node in the device tree by phandle.
pub fn find_node_by_phandle(handle: PHandle) -> Option<Node> {
    #[cfg(not(CONFIG_OF))]
    {
        None
    }
    #[cfg(CONFIG_OF)]
    // SAFETY: bindings::of_find_node_by_phandle always returns a valid pointer or NULL
    unsafe {
        #[allow(dead_code)]
        Node::from_raw(bindings::of_find_node_by_phandle(handle))
    }
}

impl Clone for Node {
    fn clone(&self) -> Node {
        // SAFETY: `raw_node` is valid and non-NULL per the type invariant,
        // so this can never return None.
        unsafe { Node::get_from_raw(self.raw_node).unwrap() }
    }
}

impl Drop for Node {
    fn drop(&mut self) {
        #[cfg(CONFIG_OF_DYNAMIC)]
        // SAFETY: `raw_node` is valid per the type invariant.
        unsafe {
            bindings::of_node_put(self.raw_node)
        };
    }
}

/// Create an OF `IdTable` with an "alias" for modpost.
#[macro_export]
macro_rules! of_device_table {
    ($table_name:ident, $module_table_name:ident, $id_info_type: ty, $table_data: expr) => {
        const $table_name: $crate::device_id::IdArray<
            $crate::of::DeviceId,
            $id_info_type,
            { $table_data.len() },
        > = $crate::device_id::IdArray::new($table_data);

        $crate::module_device_table!("of", $module_table_name, $table_name);
    };
}
