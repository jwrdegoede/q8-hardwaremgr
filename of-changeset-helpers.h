#ifndef __OF_CHANGESET_HELPERS_H__
#define __OF_CHANGESET_HELPERS_H__

#include <linux/of.h>

/* HACK to not build these against my sunxi-wip tree */
#ifndef OF_HAVE_CHANGESET_HELPERS

/*
 * Copied from drivers/of/base.c and drivers/of/dynamic.c
 * for use in changeset helpers, this entire file should be dropped
 * once the changeset helpers have been merged upstream.
 */

/*
 * HACK HACK HACK we need this and it is not exported by the kernel, since
 * any nodes we add never get released, we simply omit the release method.
 */
struct kobj_type of_node_ktype = { };

/**
 * __of_add_property - Add a property to a node without lock operations
 */
static int __of_add_property(struct device_node *np, struct property *prop)
{
	struct property **next;

	prop->next = NULL;
	next = &np->properties;
	while (*next) {
		if (strcmp(prop->name, (*next)->name) == 0)
			/* duplicate ! don't insert it */
			return -EEXIST;

		next = &(*next)->next;
	}
	*next = prop;

	return 0;
}

/**
 * __of_prop_dup - Copy a property dynamically.
 * @prop:	Property to copy
 * @allocflags:	Allocation flags (typically pass GFP_KERNEL)
 *
 * Copy a property by dynamically allocating the memory of both the
 * property structure and the property name & contents. The property's
 * flags have the OF_DYNAMIC bit set so that we can differentiate between
 * dynamically allocated properties and not.
 * Returns the newly allocated property or NULL on out of memory error.
 */
static struct property *__of_prop_dup(const struct property *prop, gfp_t allocflags)
{
	struct property *new;

	new = kzalloc(sizeof(*new), allocflags);
	if (!new)
		return NULL;

	/*
	 * NOTE: There is no check for zero length value.
	 * In case of a boolean property, this will allocate a value
	 * of zero bytes. We do this to work around the use
	 * of of_get_property() calls on boolean values.
	 */
	new->name = kstrdup(prop->name, allocflags);
	new->value = kmemdup(prop->value, prop->length, allocflags);
	new->length = prop->length;
	if (!new->name || !new->value)
		goto err_free;

	/* mark the property as dynamic */
	of_property_set_flag(new, OF_DYNAMIC);

	return new;

 err_free:
	kfree(new->name);
	kfree(new->value);
	kfree(new);
	return NULL;
}

/**
 * __of_node_dupv() - Duplicate or create an empty device node dynamically.
 * @fmt: Format string for new full name of the device node
 * @vargs: va_list containing the arugments for the node full name
 *
 * Create an device tree node, either by duplicating an empty node or by allocating
 * an empty one suitable for further modification.  The node data are
 * dynamically allocated and all the node flags have the OF_DYNAMIC &
 * OF_DETACHED bits set. Returns the newly allocated node or NULL on out of
 * memory error.
 */
static struct device_node *__of_node_dupv(const struct device_node *np,
		const char *fmt, va_list vargs)
{
	struct device_node *node;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return NULL;
	node->full_name = kvasprintf(GFP_KERNEL, fmt, vargs);
	if (!node->full_name) {
		kfree(node);
		return NULL;
	}

	of_node_set_flag(node, OF_DYNAMIC);
	of_node_set_flag(node, OF_DETACHED);
	of_node_init(node);

	/* Iterate over and duplicate all properties */
	if (np) {
		struct property *pp, *new_pp;
		for_each_property_of_node(np, pp) {
			new_pp = __of_prop_dup(pp, GFP_KERNEL);
			if (!new_pp)
				goto err_prop;
			if (__of_add_property(node, new_pp)) {
				kfree(new_pp->name);
				kfree(new_pp->value);
				kfree(new_pp);
				goto err_prop;
			}
		}
	}
	return node;

 err_prop:
	of_node_put(node); /* Frees the node and properties */
	return NULL;
}

/* changeset helpers */

/**
 * of_changeset_create_device_node - Create an empty device node
 *
 * @ocs:	changeset pointer
 * @parent:	parent device node
 * @fmt:	format string for the node's full_name
 * @args:	argument list for the format string
 *
 * Create an empty device node, marking it as detached and allocated.
 *
 * Returns a device node on success, an error encoded pointer otherwise
 */
static inline struct device_node *of_changeset_create_device_nodev(
	struct of_changeset *ocs, struct device_node *parent,
	const char *fmt, va_list vargs)
{
	struct device_node *node;

	node = __of_node_dupv(NULL, fmt, vargs);
	if (!node)
		return ERR_PTR(-ENOMEM);

	node->parent = parent;
	return node;
}

/**
 * of_changeset_create_device_node - Create an empty device node
 *
 * @ocs:	changeset pointer
 * @parent:	parent device node
 * @fmt:	Format string for the node's full_name
 * ...		Arguments
 *
 * Create an empty device node, marking it as detached and allocated.
 *
 * Returns a device node on success, an error encoded pointer otherwise
 */
static struct device_node *of_changeset_create_device_node(
	struct of_changeset *ocs, struct device_node *parent,
	const char *fmt, ...)
{
	va_list vargs;
	struct device_node *node;

	va_start(vargs, fmt);
	node = of_changeset_create_device_nodev(ocs, parent, fmt, vargs);
	va_end(vargs);
	return node;
}

static int __of_changeset_add_update_property_copy(struct of_changeset *ocs,
		struct device_node *np, const char *name, const void *value,
		int length, bool update)
{
	struct property *prop;
	int ret = -ENOMEM;

	prop = kzalloc(sizeof(*prop), GFP_KERNEL);
	if (!prop)
		return ret;

	prop->name = kstrdup(name, GFP_KERNEL);
	if (!prop->name)
		goto out;

	/*
	 * NOTE: There is no check for zero length value.
	 * In case of a boolean property, this will allocate a value
	 * of zero bytes. We do this to work around the use
	 * of of_get_property() calls on boolean values.
	 */
	prop->value = kmemdup(value, length, GFP_KERNEL);
	if (!prop->value)
		goto out;

	of_property_set_flag(prop, OF_DYNAMIC);
	prop->length = length;

	if (!update)
		ret = of_changeset_add_property(ocs, np, prop);
	else
		ret = of_changeset_update_property(ocs, np, prop);

	if (ret == 0)
		return 0;

out:
	kfree(prop->value);
	kfree(prop->name);
	kfree(prop);
	return ret;
}

static int __of_changeset_add_update_property_string(struct of_changeset *ocs,
		struct device_node *np, const char *name, const char *str,
		bool update)
{
	return __of_changeset_add_update_property_copy(ocs, np, name, str,
			strlen(str) + 1, update);
}

static int __of_changeset_add_update_property_stringv(struct of_changeset *ocs,
		struct device_node *np, const char *name,
		const char *fmt, va_list vargs, bool update)
{
	char *str;
	int ret;

	str = kvasprintf(GFP_KERNEL, fmt, vargs);
	if (!str)
		return -ENOMEM;

	ret = __of_changeset_add_update_property_string(ocs, np, name, str, update);

	kfree(str);

	return ret;
}

static int __of_changeset_add_update_property_string_list(
		struct of_changeset *ocs, struct device_node *np, const char *name,
		const char **strs, int count, bool update)
{
	int total = 0, i, ret;
	char *value, *s;

	for (i = 0; i < count; i++) {
		/* check if  it's NULL */
		if (!strs[i])
			return -EINVAL;
		total += strlen(strs[i]) + 1;
	}

	value = kmalloc(total, GFP_KERNEL);
	if (!value)
		return -ENOMEM;

	for (i = 0, s = value; i < count; i++) {
		/* no need to check for NULL, check above */
		strcpy(s, strs[i]);
		s += strlen(strs[i]) + 1;
	}

	ret = __of_changeset_add_update_property_copy(ocs, np, name, value,
			total, update);

	kfree(value);

	return ret;
}

/**
 * of_changeset_add_property_copy - Create a new property copying name & value
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @value:	pointer to the value data
 * @length:	length of the value in bytes
 *
 * Adds a property to the changeset by making copies of the name & value
 * entries.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_add_property_copy(struct of_changeset *ocs,
		struct device_node *np, const char *name, const void *value,
		int length)
{
	return __of_changeset_add_update_property_copy(ocs, np, name, value,
			length, false);
}

/**
 * of_changeset_add_property_string - Create a new string property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @str:	string property
 *
 * Adds a string property to the changeset by making copies of the name
 * and the string value.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_add_property_string(struct of_changeset *ocs,
		struct device_node *np, const char *name, const char *str)
{
	return __of_changeset_add_update_property_string(ocs, np, name, str,
			false);
}

/**
 * of_changeset_add_property_stringf - Create a new formatted string property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @fmt:	format of string property
 * ...		arguments of the format string
 *
 * Adds a string property to the changeset by making copies of the name
 * and the formatted value.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_add_property_stringf(struct of_changeset *ocs,
		struct device_node *np, const char *name, const char *fmt, ...)
{
	va_list vargs;
	int ret;

	va_start(vargs, fmt);
	ret = __of_changeset_add_update_property_stringv(ocs, np, name, fmt,
			vargs, false);
	va_end(vargs);
	return ret;
}

/**
 * of_changeset_add_property_string_list - Create a new string list property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @strs:	pointer to the string list
 * @count:	string count
 *
 * Adds a string list property to the changeset.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_add_property_string_list(struct of_changeset *ocs,
		struct device_node *np, const char *name, const char **strs,
		int count)
{
	return __of_changeset_add_update_property_string_list(ocs, np, name,
			strs, count, false);
}

/**
 * of_changeset_add_property_u32 - Create a new u32 property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @val:	value in host endian format
 *
 * Adds a u32 property to the changeset.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_add_property_u32(struct of_changeset *ocs,
		struct device_node *np, const char *name, u32 val)
{
	__be32 _val = cpu_to_be32(val);
	return __of_changeset_add_update_property_copy(ocs, np, name, &_val,
			sizeof(_val), false);
}

/**
 * of_changeset_add_property_bool - Create a new u32 property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 *
 * Adds a bool property to the changeset. Note that there is
 * no option to set the value to false, since the property
 * existing sets it to true.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_add_property_bool(struct of_changeset *ocs,
		struct device_node *np, const char *name)
{
	return __of_changeset_add_update_property_copy(ocs, np, name, "", 0,
			false);
}

/**
 * of_changeset_update_property_copy - Update a property copying name & value
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @value:	pointer to the value data
 * @length:	length of the value in bytes
 *
 * Update a property to the changeset by making copies of the name & value
 * entries.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_update_property_copy(struct of_changeset *ocs,
		struct device_node *np, const char *name, const void *value,
		int length)
{
	return __of_changeset_add_update_property_copy(ocs, np, name, value,
			length, true);
}

/**
 * of_changeset_update_property_string - Create a new string property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @str:	string property
 *
 * Updates a string property to the changeset by making copies of the name
 * and the string value.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_update_property_string(struct of_changeset *ocs,
		struct device_node *np, const char *name, const char *str)
{
	return __of_changeset_add_update_property_string(ocs, np, name, str,
			true);
}

/**
 * of_changeset_update_property_stringf - Update formatted string property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @fmt:	format of string property
 * ...		arguments of the format string
 *
 * Updates a string property to the changeset by making copies of the name
 * and the formatted value.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_update_property_stringf(struct of_changeset *ocs,
		struct device_node *np, const char *name, const char *fmt, ...)
{
	va_list vargs;
	int ret;

	va_start(vargs, fmt);
	ret = __of_changeset_add_update_property_stringv(ocs, np, name, fmt,
			vargs, true);
	va_end(vargs);
	return ret;
}

/**
 * of_changeset_update_property_string_list - Update string list property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @strs:	pointer to the string list
 * @count:	string count
 *
 * Updates a string list property to the changeset.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_update_property_string_list(struct of_changeset *ocs,
		struct device_node *np, const char *name, const char **strs,
		int count)
{
	return __of_changeset_add_update_property_string_list(ocs, np, name,
			strs, count, true);
}

/**
 * of_changeset_update_property_u32 - Update u32 property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 * @val:	value in host endian format
 *
 * Updates a u32 property to the changeset.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_update_property_u32(struct of_changeset *ocs,
		struct device_node *np, const char *name, u32 val)
{
	__be32 _val = cpu_to_be32(val);
	return __of_changeset_add_update_property_copy(ocs, np, name, &_val,
			sizeof(_val), true);
}

/**
 * of_changeset_update_property_bool - Update a bool property
 *
 * @ocs:	changeset pointer
 * @np:		device node pointer
 * @name:	name of the property
 *
 * Updates a property to the changeset. Note that there is
 * no option to set the value to false, since the property
 * existing sets it to true.
 *
 * Returns zero on success, a negative error value otherwise.
 */
static inline int of_changeset_update_property_bool(struct of_changeset *ocs,
		struct device_node *np, const char *name)
{
	return __of_changeset_add_update_property_copy(ocs, np, name, "", 0,
			true);
}

#endif

static inline phandle of_gen_phandle(void)
{
	struct device_node *np;
	phandle phandle = 0xdeadbeaf;

	while ((np = of_find_node_by_phandle(phandle))) {
		of_node_put(np);
		phandle++;
	}

	return phandle;
}

#endif /* ifndef __OF_CHANGESET_HELPERS_H__ */
