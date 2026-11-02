---
layout: default
title: C++ script limits and errors
nav_order: 27
group: C++ scripts
---

This page lists the forms that a `.cpp` script does not support, the messages of `hotc` and the known limits.

## Unsupported forms

A class with no base is not a script class. `hotc` copies it unchanged, like a struct without a base. It has no hot reload.

The first version gives an error for each form in this list. The text in the second column is the message of `hotc`.

| Form | Message |
|---|---|
| A script class or a module variable in an anonymous namespace | `an anonymous namespace cannot hold a script class or a module variable in a .cpp script; use a named namespace` |
| A script class or a module variable in an inline namespace | `an inline namespace cannot hold a script class or a module variable in a .cpp script; remove 'inline'` |
| A convert hook for a module variable of a namespace, written inside a namespace | `the convert hook of the module variable '<ns::var>' must be at file scope and must be named 'convert_<ns__var>'` |
| Two classes or two module variables with the same name after the join with `__` | `'<a>' and '<b>' have the same generated name '<name>'. Rename one of them` |
| A script class that derives from a class of the file that is not a script class | `a .cpp script class cannot derive from '<name>': it is a plain class of the script; derive from a script class or from a native class` |
| A script class that derives from a script class of another `.cpp` file | `a .cpp script class cannot derive from '<name>' of another .cpp file; a script class can derive only from a script class of the same file` |
| A class in a function or in another class that derives from a script class | `the class '<name>' cannot derive from the script class '<parent>': only a class at file scope can derive from a script class in a .cpp script` |
| A script class with a script parent and one more base class | `a .cpp script class with the base the script class '<name>' cannot have another base class` |
| A script parent with `virtual`, for example `class Kid : public virtual Base` | `a .cpp script class cannot derive from the script class '<name>' with 'virtual'` |
| A script parent with `protected` or `private` | `a .cpp script class must derive from the script class '<name>' with 'public'` |
| `virtual` without `override` on a method that overrides a virtual method | `the method '<name>' overrides a virtual method; write 'override'` |
| A static member used through an object that has a side effect, for example `f().count` | `a static member must be used by its name` |
| An instance method `s_x` and a static member `x` in one class (both get the name `<Class>__s_x`) | `the method '<Class>::s_x' and the static member '<Class>::x' have the same generated name '<Class>__s_x'. Rename one of them` |
| A static const member without an initializer in the class | `a static const member needs an initializer in the class` |
| A method, a constructor or a destructor that the class declares and the file never defines | `the method 'Player::hit' is declared but never defined; define it in the class or after the class` (the words `constructor` and `destructor` replace `method` for those members) |
| A definition outside the class that has no declaration in the class | `out-of-line definition of 'hit' does not match any declaration in 'Player'` (this error comes from the C++ parser) |
| A static method that is declared in the class and defined outside it | `define the static method inside the class` |
| A body outside the class that names a declaration which the file puts after the class | `the definition of 'Player::hit' uses 'kLate', which the file declares after the class; the body moves into the class, so declare 'kLate' before the class` |
| A pure virtual method (`= 0`) | `a pure virtual method is not supported in a .cpp script class` |
| A constructor, a destructor or a method that is `= default` or `= delete` | `a defaulted or deleted method is not supported in a .cpp script class` (the word `constructor` replaces `method` for a constructor) |
| A `const` field or a reference field in the initializer list | `the const field '<name>' cannot be in the initializer list: a field lives in the state; use a default member initializer` (the words `reference field` replace `const field` for a reference) |
| An array field in the initializer list | `the array field '<name>' cannot be in the initializer list; assign its elements in the body` |
| A delegating constructor, for example `Player() : Player(1) {}` | `a delegating constructor is not supported in a .cpp script class` |
| A default member initializer that reads a field which has an entry in the list | `the default member initializer of the field '<b>' reads the field '<a>', which this constructor sets in its initializer list; ...` |
| An entry in the initializer list that is not a field of the class, or a field that has two entries | An error of the C++ parser, for example `member initializer 'x' does not name a non-static data member or base class` |
| Any other declaration in the class | `this declaration is not supported in a .cpp script class` |
| Properties | There is no property syntax in C++. Use two methods. |
| Script-class handles | A script class is not a handle in a `.cpp` script. Use a pointer. See [Script classes as types](cpp-types.html#script-classes-as-types). |
| A reference field or a reference module variable, for example `Mesh& r;` | `a field cannot be a reference: a reference cannot change its target and has no null. Use 'T*' (a checked pointer) or a value` |
| A pointer to a builtin type or to an enum, for example `int* p = nullptr;` | `a field cannot hold a pointer to the value type 'int': nothing owns it, so the pointer can dangle. Use a value, or a pointer to the object that owns it` |
| A pointer field of a native class with no weak source, for example `Mesh* m = nullptr;` | `a field cannot hold a borrowed pointer or reference to 'Mesh'. Declare a stub with 'handle' or 'intrusive' for 'Mesh', or use a value` |
| `delete p` on a native pointer field | Not supported. The field is a `hot::CheckedPtr`. The code compiles through its `operator T*`, but the weak pointer does not own the object. Do not use `delete` on it. |
| A script class by value, for example `Minion m;` or a parameter `Minion m` | `the script class '<name>' by value is not supported in a .cpp script; use a pointer or a reference` |
| An array of a script class, for example `Minion a[2];` | `an array of the script class '<name>' is not supported in a .cpp script; use a pointer or a reference` |
| A script class as a template argument, for example `std::vector<Minion>` | `the script class '<name>' as a template argument is not supported in a .cpp script; use a pointer or a reference` |
| `typeid` of a script class or of a script object | `'typeid' of the script class '<name>' is not supported in a .cpp script: a script class has no C++ RTTI; use dynamic_cast` |
| A placement `new` of a script class | `a placement 'new' of the script class '<name>' is not supported in a .cpp script` |
| An array `new` of a script class, for example `new Minion[2]` | `an array 'new' of the script class '<name>' is not supported in a .cpp script; use an array of pointers` |
| `Minion::` before a member that is not static, on another object or outside a child class, for example `p->Minion::boost(1)` | `the script class '<name>' can qualify only a static member, or a member that a child class uses on itself, in a .cpp script` (outside a script class the end of the message is `can qualify only a static member in a .cpp script`) |
| A forward declaration of a script class | `a forward declaration of the script class '<name>' is not supported in a .cpp script; define the class before its first use` |
| A lambda that captures `*this` | `a lambda cannot capture '*this' in a .cpp script: it would copy the object; capture a copy of the fields` |
| A module variable that uses a type of the file that comes after the first module variable | `the type '<type>' must come before the first module variable '<name>': the module block is there and needs the complete type` |
| A module variable whose initializer uses a function of the file that comes after the first module variable | `'<function>' must come before the first module variable '<name>': the module block is there and holds the initializers` |
| A variable that is not a module variable and uses a module variable in its initializer | `the variable '<variable>' cannot use the module variable '<name>' in a .cpp script: it gets its value before the module state exists` |
| A base member hidden by a macro | `a macro hides a member of the base class; write the member in the script text` |
| An include of a `.hot` script | `a .cpp script cannot include a .hot script` |

For a `const` field or a reference field, use a default member initializer in the class body. For a delegating constructor, write the shared code in a method.

## Known limits

- A field of a native struct type that has a function pointer can point into the code of an old version. `hotc` finds such a struct in an included header, wherever the `#include` is. Then the old version stays loaded while a live object has the field. `hotc` does not find a struct that you define in the script file itself.
- A field type that has no copy and no move constructor, for example `std::atomic<int>`, is built in place. A reload cannot carry its value. The new object runs the initializer of the field again. A move-only field, for example `std::unique_ptr`, of the old object is moved-from after a failed migration.
- An in-class brace initializer of a class-type field is not visited by `libclang`. A module variable inside such an initializer is not rewritten.
- A `.cpp` translation does one more `libclang` parse than a `.hot` translation. A reload is slower. One parse of the engine headers takes about 400 ms.
- In a normal build the file has plain C++ meaning. There is no state migration and there are no slots. Objects must not be shared between the two builds.
- The language server gives only the errors of `hotc` for a `.cpp` script, and only when a host lists the file. The other editor features come from `clangd`. For a script with aids, `clangd` needs the flags `-include hot/script.h` and `-I<hot>/include`. See [Tools](tools.html#a-cpp-script-in-the-editor).
