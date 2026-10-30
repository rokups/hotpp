// C++17 forms of C++20 library traits and attributes. The generated code and the runtime
// headers need C++17 only. Do not use a feature of a later standard in include/spp.
#pragma once
#include <memory>
#include <type_traits>
#include <utility>

// A branch hint: the condition is usually false. C++20 has [[unlikely]].
#if defined(__clang__) || defined(__GNUC__)
#define SPP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define SPP_UNLIKELY(x) (x)
#endif

namespace spp::detail {

// std::remove_cvref_t (C++20).
template <class T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

// std::type_identity_t (C++20). A parameter of this type does not take part in deduction.
template <class T>
struct type_identity {
    using type = T;
};
template <class T>
using type_identity_t = typename type_identity<T>::type;

// True if "h.get()" is valid for "const T& h": a handle type (std::shared_ptr, Intrusive).
template <class T, class = void>
struct has_get : std::false_type {};
template <class T>
struct has_get<T, std::void_t<decltype(std::declval<const T&>().get())>> : std::true_type {};
template <class T>
inline constexpr bool has_get_v = has_get<T>::value;

// True if T has a member type "element_type".
template <class T, class = void>
struct has_element_type : std::false_type {};
template <class T>
struct has_element_type<T, std::void_t<typename T::element_type>> : std::true_type {};

// True if "p->shared_from_this()" is valid for "T* p".
template <class T, class = void>
struct has_shared_from_this : std::false_type {};
template <class T>
struct has_shared_from_this<T, std::void_t<decltype(std::declval<T*>()->shared_from_this())>> : std::true_type {};

// True if "d.Bind(std::move(f))" is valid for "D& d" and "F f": an engine delegate type.
template <class D, class F, class = void>
struct has_bind : std::false_type {};
template <class D, class F>
struct has_bind<D, F, std::void_t<decltype(std::declval<D&>().Bind(std::declval<F>()))>> : std::true_type {};

}  // namespace spp::detail
