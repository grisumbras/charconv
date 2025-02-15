// Copyright 2020-2023 Junekey Jeon
// Copyright 2022 Peter Dimov
// Copyright 2023 Matt Borland
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/charconv/to_chars.hpp>
#include <boost/charconv/chars_format.hpp>
#include <limits>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cmath>

namespace boost { namespace charconv { namespace detail { namespace to_chars_detail {

#ifdef BOOST_MSVC
# pragma warning(push)
# pragma warning(disable: 4127) // Conditional expression is constant (e.g. BOOST_IF_CONSTEXPR statements)
#endif

    // These "//"'s are to prevent clang-format to ruin this nice alignment.
    // Thanks to reddit user u/mcmcc:
    // https://www.reddit.com/r/cpp/comments/so3wx9/dragonbox_110_is_released_a_fast_floattostring/hw8z26r/?context=3
    static constexpr char radix_100_head_table[] = {
        '0', '.', '1', '.', '2', '.', '3', '.', '4', '.', //
        '5', '.', '6', '.', '7', '.', '8', '.', '9', '.', //
        '1', '.', '1', '.', '1', '.', '1', '.', '1', '.', //
        '1', '.', '1', '.', '1', '.', '1', '.', '1', '.', //
        '2', '.', '2', '.', '2', '.', '2', '.', '2', '.', //
        '2', '.', '2', '.', '2', '.', '2', '.', '2', '.', //
        '3', '.', '3', '.', '3', '.', '3', '.', '3', '.', //
        '3', '.', '3', '.', '3', '.', '3', '.', '3', '.', //
        '4', '.', '4', '.', '4', '.', '4', '.', '4', '.', //
        '4', '.', '4', '.', '4', '.', '4', '.', '4', '.', //
        '5', '.', '5', '.', '5', '.', '5', '.', '5', '.', //
        '5', '.', '5', '.', '5', '.', '5', '.', '5', '.', //
        '6', '.', '6', '.', '6', '.', '6', '.', '6', '.', //
        '6', '.', '6', '.', '6', '.', '6', '.', '6', '.', //
        '7', '.', '7', '.', '7', '.', '7', '.', '7', '.', //
        '7', '.', '7', '.', '7', '.', '7', '.', '7', '.', //
        '8', '.', '8', '.', '8', '.', '8', '.', '8', '.', //
        '8', '.', '8', '.', '8', '.', '8', '.', '8', '.', //
        '9', '.', '9', '.', '9', '.', '9', '.', '9', '.', //
        '9', '.', '9', '.', '9', '.', '9', '.', '9', '.'  //
    };

    static void print_1_digit(std::uint32_t n, char* buffer) noexcept
    {
        *buffer = char('0' + n);
    }

    static void print_2_digits(std::uint32_t n, char* buffer) noexcept 
    {
        std::memcpy(buffer, radix_table + n * 2, 2);
    }

    // These digit generation routines are inspired by James Anhalt's itoa algorithm:
    // https://github.com/jeaiii/itoa
    // The main idea is for given n, find y such that floor(10^k * y / 2^32) = n holds,
    // where k is an appropriate integer depending on the length of n.
    // For example, if n = 1234567, we set k = 6. In this case, we have
    // floor(y / 2^32) = 1,
    // floor(10^2 * ((10^0 * y) mod 2^32) / 2^32) = 23,
    // floor(10^2 * ((10^2 * y) mod 2^32) / 2^32) = 45, and
    // floor(10^2 * ((10^4 * y) mod 2^32) / 2^32) = 67.
    // See https://jk-jeon.github.io/posts/2022/02/jeaiii-algorithm/ for more explanation.

    BOOST_FORCEINLINE static void print_9_digits(std::uint32_t s32, int& exponent,
                                                char*& buffer) noexcept 
    {
        // -- IEEE-754 binary32
        // Since we do not cut trailing zeros in advance, s32 must be of 6~9 digits
        // unless the original input was subnormal.
        // In particular, when it is of 9 digits it shouldn't have any trailing zeros.
        // -- IEEE-754 binary64
        // In this case, s32 must be of 7~9 digits unless the input is subnormal,
        // and it shouldn't have any trailing zeros if it is of 9 digits.
        if (s32 >= 100000000)
        {
            // 9 digits.
            // 1441151882 = ceil(2^57 / 1'0000'0000) + 1
            auto prod = s32 * std::uint64_t(1441151882);
            prod >>= 25;
            std::memcpy(buffer, radix_100_head_table + std::uint32_t(prod >> 32) * 2, 2);

            prod = std::uint32_t(prod) * std::uint64_t(100);
            print_2_digits(std::uint32_t(prod >> 32), buffer + 2);
            prod = std::uint32_t(prod) * std::uint64_t(100);
            print_2_digits(std::uint32_t(prod >> 32), buffer + 4);
            prod = std::uint32_t(prod) * std::uint64_t(100);
            print_2_digits(std::uint32_t(prod >> 32), buffer + 6);
            prod = std::uint32_t(prod) * std::uint64_t(100);
            print_2_digits(std::uint32_t(prod >> 32), buffer + 8);

            exponent += 8;
            buffer += 10;
        }
        else if (s32 >= 1000000) 
        {
            // 7 or 8 digits.
            // 281474978 = ceil(2^48 / 100'0000) + 1
            auto prod = s32 * std::uint64_t(281474978);
            prod >>= 16;
            const auto head_digits = std::uint32_t(prod >> 32);
            // If s32 is of 8 digits, increase the exponent by 7.
            // Otherwise, increase it by 6.
            exponent += static_cast<int>(6 + unsigned(head_digits >= 10));

            // Write the first digit and the decimal point.
            std::memcpy(buffer, radix_100_head_table + head_digits * 2, 2);
            // This third character may be overwritten later, but we don't care.
            buffer[2] = radix_table[head_digits * 2 + 1];

            // Remaining 6 digits are all zero?
            if (std::uint32_t(prod) <= std::uint32_t((std::uint64_t(1) << 32) / 1000000)) 
            {
                // The number of characters actually need to be written is:
                //   1, if only the first digit is nonzero, which means that either s32 is of 7
                //   digits or it is of 8 digits but the second digit is zero, or
                //   3, otherwise.
                // Note that buffer[2] is never '0' if s32 is of 7 digits, because the input is
                // never zero.
                buffer += (1 + (unsigned(head_digits >= 10) & unsigned(buffer[2] > '0')) * 2);
            }
            else 
            {
                // At least one of the remaining 6 digits are nonzero.
                // After this adjustment, now the first destination becomes buffer + 2.
                buffer += unsigned(head_digits >= 10);

                // Obtain the next two digits.
                prod = std::uint32_t(prod) * std::uint64_t(100);
                print_2_digits(std::uint32_t(prod >> 32), buffer + 2);

                // Remaining 4 digits are all zero?
                if (std::uint32_t(prod) <= std::uint32_t((std::uint64_t(1) << 32) / 10000)) 
                {
                    buffer += (3 + unsigned(buffer[3] > '0'));
                }
                else 
                {
                    // At least one of the remaining 4 digits are nonzero.

                    // Obtain the next two digits.
                    prod = std::uint32_t(prod) * std::uint64_t(100);
                    print_2_digits(std::uint32_t(prod >> 32), buffer + 4);

                    // Remaining 2 digits are all zero?
                    if (std::uint32_t(prod) <= std::uint32_t((std::uint64_t(1) << 32) / 100))
                    {
                        buffer += (5 + unsigned(buffer[5] > '0'));
                    }
                    else 
                    {
                        // Obtain the last two digits.
                        prod = std::uint32_t(prod) * std::uint64_t(100);
                        print_2_digits(std::uint32_t(prod >> 32), buffer + 6);

                        buffer += (7 + unsigned(buffer[7] > '0'));
                    }
                }
            }
        }
        else if (s32 >= 10000)
        {
            // 5 or 6 digits.
            // 429497 = ceil(2^32 / 1'0000)
            auto prod = s32 * std::uint64_t(429497);
            const auto head_digits = std::uint32_t(prod >> 32);

            // If s32 is of 6 digits, increase the exponent by 5.
            // Otherwise, increase it by 4.
            exponent += static_cast<int>(4 + unsigned(head_digits >= 10));

            // Write the first digit and the decimal point.
            std::memcpy(buffer, radix_100_head_table + head_digits * 2, 2);
            // This third character may be overwritten later but we don't care.
            buffer[2] = radix_table[head_digits * 2 + 1];

            // Remaining 4 digits are all zero?
            if (std::uint32_t(prod) <= std::uint32_t((std::uint64_t(1) << 32) / 10000)) 
            {
                // The number of characters actually written is 1 or 3, similarly to the case of
                // 7 or 8 digits.
                buffer += (1 + (unsigned(head_digits >= 10) & unsigned(buffer[2] > '0')) * 2);
            }
            else 
            {
                // At least one of the remaining 4 digits are nonzero.
                // After this adjustment, now the first destination becomes buffer + 2.
                buffer += unsigned(head_digits >= 10);

                // Obtain the next two digits.
                prod = std::uint32_t(prod) * std::uint64_t(100);
                print_2_digits(std::uint32_t(prod >> 32), buffer + 2);

                // Remaining 2 digits are all zero?
                if (std::uint32_t(prod) <= std::uint32_t((std::uint64_t(1) << 32) / 100))
                {
                    buffer += (3 + unsigned(buffer[3] > '0'));
                }
                else
                {
                    // Obtain the last two digits.
                    prod = std::uint32_t(prod) * std::uint64_t(100);
                    print_2_digits(std::uint32_t(prod >> 32), buffer + 4);

                    buffer += (5 + unsigned(buffer[5] > '0'));
                }
            }
        }
        else if (s32 >= 100)
        {
            // 3 or 4 digits.
            // 42949673 = ceil(2^32 / 100)
            auto prod = s32 * std::uint64_t(42949673);
            const auto head_digits = std::uint32_t(prod >> 32);

            // If s32 is of 4 digits, increase the exponent by 3.
            // Otherwise, increase it by 2.
            exponent += (2 + int(head_digits >= 10));

            // Write the first digit and the decimal point.
            std::memcpy(buffer, radix_100_head_table + head_digits * 2, 2);
            // This third character may be overwritten later but we don't care.
            buffer[2] = radix_table[head_digits * 2 + 1];

            // Remaining 2 digits are all zero?
            if (std::uint32_t(prod) <= std::uint32_t((std::uint64_t(1) << 32) / 100))
            {
                // The number of characters actually written is 1 or 3, similarly to the case of
                // 7 or 8 digits.
                buffer += (1 + (unsigned(head_digits >= 10) & unsigned(buffer[2] > '0')) * 2);
            }
            else
            {
                // At least one of the remaining 2 digits are nonzero.
                // After this adjustment, now the first destination becomes buffer + 2.
                buffer += unsigned(head_digits >= 10);

                // Obtain the last two digits.
                prod = std::uint32_t(prod) * std::uint64_t(100);
                print_2_digits(std::uint32_t(prod >> 32), buffer + 2);

                buffer += (3 + unsigned(buffer[3] > '0'));
            }
        }
        else
        {
            // 1 or 2 digits.
            // If s32 is of 2 digits, increase the exponent by 1.
            exponent += int(s32 >= 10);

            // Write the first digit and the decimal point.
            std::memcpy(buffer, radix_100_head_table + s32 * 2, 2);
            // This third character may be overwritten later but we don't care.
            buffer[2] = radix_table[s32 * 2 + 1];

            // The number of characters actually written is 1 or 3, similarly to the case of
            // 7 or 8 digits.
            buffer += (1 + (unsigned(s32 >= 10) & unsigned(buffer[2] > '0')) * 2);
        }
    }

    template <>
    char* to_chars<float, dragonbox_float_traits<float>>(std::uint32_t s32, int exponent, char* buffer, chars_format fmt) noexcept
    {
        // Print significand.
        print_9_digits(s32, exponent, buffer);

        // Print exponent and return
        if (exponent < 0)
        {
            std::memcpy(buffer, "e-", 2);
            buffer += 2;
            exponent = -exponent;
        }
        else if (exponent == 0)
        {
            if (fmt == chars_format::scientific)
            {
                std::memcpy(buffer, "e+00", 4);
                buffer += 4;
            }

            return buffer;
        }
        else 
        {
            std::memcpy(buffer, "e+", 2);
            buffer += 2;
        }

        print_2_digits(std::uint32_t(exponent), buffer);
        buffer += 2;

        return buffer;
    }

    template <>
    char* to_chars<double, dragonbox_float_traits<double>>(const std::uint64_t significand, int exponent, char* buffer, chars_format fmt) noexcept {
        // Print significand by decomposing it into a 9-digit block and a 8-digit block.
        std::uint32_t first_block;
        std::uint32_t second_block {};
        bool no_second_block;

        if (significand >= 100000000)
        {
            first_block = std::uint32_t(significand / 100000000);
            second_block = std::uint32_t(significand) - first_block * 100000000;
            exponent += 8;
            no_second_block = (second_block == 0);
        }
        else
        {
            first_block = std::uint32_t(significand);
            no_second_block = true;
        }

        if (no_second_block)
        {
            print_9_digits(first_block, exponent, buffer);
        }
        else
        {
            // We proceed similarly to print_9_digits(), but since we do not need to remove
            // trailing zeros, the procedure is a bit simpler.
            if (first_block >= 100000000)
            {
                // The input is of 17 digits, thus there should be no trailing zero at all.
                // The first block is of 9 digits.
                // 1441151882 = ceil(2^57 / 1'0000'0000) + 1
                auto prod = first_block * std::uint64_t(1441151882);
                prod >>= 25;
                std::memcpy(buffer, radix_100_head_table + std::uint32_t(prod >> 32) * 2, 2);
                prod = std::uint32_t(prod) * std::uint64_t(100);
                print_2_digits(std::uint32_t(prod >> 32), buffer + 2);
                prod = std::uint32_t(prod) * std::uint64_t(100);
                print_2_digits(std::uint32_t(prod >> 32), buffer + 4);
                prod = std::uint32_t(prod) * std::uint64_t(100);
                print_2_digits(std::uint32_t(prod >> 32), buffer + 6);
                prod = std::uint32_t(prod) * std::uint64_t(100);
                print_2_digits(std::uint32_t(prod >> 32), buffer + 8);

                // The second block is of 8 digits.
                // 281474978 = ceil(2^48 / 100'0000) + 1
                prod = second_block * std::uint64_t(281474978);
                prod >>= 16;
                prod += 1;
                print_2_digits(std::uint32_t(prod >> 32), buffer + 10);
                prod = std::uint32_t(prod) * std::uint64_t(100);
                print_2_digits(std::uint32_t(prod >> 32), buffer + 12);
                prod = std::uint32_t(prod) * std::uint64_t(100);
                print_2_digits(std::uint32_t(prod >> 32), buffer + 14);
                prod = std::uint32_t(prod) * std::uint64_t(100);
                print_2_digits(std::uint32_t(prod >> 32), buffer + 16);

                exponent += 8;
                buffer += 18;
            }
            else
            {
                if (first_block >= 1000000)
                {
                    // 7 or 8 digits.
                    // 281474978 = ceil(2^48 / 100'0000) + 1
                    auto prod = first_block * std::uint64_t(281474978);
                    prod >>= 16;
                    const auto head_digits = std::uint32_t(prod >> 32);

                    std::memcpy(buffer, radix_100_head_table + head_digits * 2, 2);
                    buffer[2] = radix_table[head_digits * 2 + 1];

                    exponent += static_cast<int>(6 + unsigned(head_digits >= 10));
                    buffer += unsigned(head_digits >= 10);

                    // Print remaining 6 digits.
                    prod = std::uint32_t(prod) * std::uint64_t(100);
                    print_2_digits(std::uint32_t(prod >> 32), buffer + 2);
                    prod = std::uint32_t(prod) * std::uint64_t(100);
                    print_2_digits(std::uint32_t(prod >> 32), buffer + 4);
                    prod = std::uint32_t(prod) * std::uint64_t(100);
                    print_2_digits(std::uint32_t(prod >> 32), buffer + 6);

                    buffer += 8;
                }
                else if (first_block >= 10000)
                {
                    // 5 or 6 digits.
                    // 429497 = ceil(2^32 / 1'0000)
                    auto prod = first_block * std::uint64_t(429497);
                    const auto head_digits = std::uint32_t(prod >> 32);

                    std::memcpy(buffer, radix_100_head_table + head_digits * 2, 2);
                    buffer[2] = radix_table[head_digits * 2 + 1];

                    exponent += static_cast<int>(4 + unsigned(head_digits >= 10));
                    buffer += unsigned(head_digits >= 10);

                    // Print remaining 4 digits.
                    prod = std::uint32_t(prod) * std::uint64_t(100);
                    print_2_digits(std::uint32_t(prod >> 32), buffer + 2);
                    prod = std::uint32_t(prod) * std::uint64_t(100);
                    print_2_digits(std::uint32_t(prod >> 32), buffer + 4);

                    buffer += 6;
                }
                else if (first_block >= 100)
                {
                    // 3 or 4 digits.
                    // 42949673 = ceil(2^32 / 100)
                    auto prod = first_block * std::uint64_t(42949673);
                    const auto head_digits = std::uint32_t(prod >> 32);

                    std::memcpy(buffer, radix_100_head_table + head_digits * 2, 2);
                    buffer[2] = radix_table[head_digits * 2 + 1];

                    exponent += static_cast<int>(2 + unsigned(head_digits >= 10));
                    buffer += unsigned(head_digits >= 10);

                    // Print remaining 2 digits.
                    prod = std::uint32_t(prod) * std::uint64_t(100);
                    print_2_digits(std::uint32_t(prod >> 32), buffer + 2);

                    buffer += 4;
                }
                else
                {
                    // 1 or 2 digits.
                    std::memcpy(buffer, radix_100_head_table + first_block * 2, 2);
                    buffer[2] = radix_table[first_block * 2 + 1];

                    exponent += (first_block >= 10);
                    buffer += (2 + unsigned(first_block >= 10));
                }

                // Next, print the second block.
                // The second block is of 8 digits, but we may have trailing zeros.
                // 281474978 = ceil(2^48 / 100'0000) + 1
                auto prod = second_block * std::uint64_t(281474978);
                prod >>= 16;
                prod += 1;
                print_2_digits(std::uint32_t(prod >> 32), buffer);

                // Remaining 6 digits are all zero?
                if (std::uint32_t(prod) <= std::uint32_t((std::uint64_t(1) << 32) / 1000000))
                {
                    buffer += (1 + unsigned(buffer[1] > '0'));
                }
                else
                {
                    // Obtain the next two digits.
                    prod = std::uint32_t(prod) * std::uint64_t(100);
                    print_2_digits(std::uint32_t(prod >> 32), buffer + 2);

                    // Remaining 4 digits are all zero?
                    if (std::uint32_t(prod) <= std::uint32_t((std::uint64_t(1) << 32) / 10000)) 
                    {
                        buffer += (3 + unsigned(buffer[3] > '0'));
                    }
                    else
                    {
                        // Obtain the next two digits.
                        prod = std::uint32_t(prod) * std::uint64_t(100);
                        print_2_digits(std::uint32_t(prod >> 32), buffer + 4);

                        // Remaining 2 digits are all zero?
                        if (std::uint32_t(prod) <= std::uint32_t((std::uint64_t(1) << 32) / 100)) 
                        {
                            buffer += (5 + unsigned(buffer[5] > '0'));
                        }
                        else 
                        {
                            // Obtain the last two digits.
                            prod = std::uint32_t(prod) * std::uint64_t(100);
                            print_2_digits(std::uint32_t(prod >> 32), buffer + 6);
                            buffer += (7 + unsigned(buffer[7] > '0'));
                        }
                    }
                }
            }
        }
        if (exponent < 0)
        {
            std::memcpy(buffer, "e-", 2);
            buffer += 2;
            exponent = -exponent;
        }
        else if (exponent == 0)
        {
            if (fmt == chars_format::scientific)
            {
                std::memcpy(buffer, "e+00", 4);
                buffer += 4;
            }

            return buffer;
        }
        else
        {
            std::memcpy(buffer, "e+", 2);
            buffer += 2;
        }

        if (exponent >= 100) 
        {
            // d1 = exponent / 10; d2 = exponent % 10;
            // 6554 = ceil(2^16 / 10)
            auto prod = std::uint32_t(exponent) * std::uint32_t(6554);
            auto d1 = prod >> 16;
            prod = std::uint16_t(prod) * std::uint32_t(5); // * 10
            auto d2 = prod >> 15;                          // >> 16
            print_2_digits(d1, buffer);
            print_1_digit(d2, buffer + 2);
            buffer += 3;
        }
        else
        {
            print_2_digits(static_cast<std::uint32_t>(exponent), buffer);
            buffer += 2;
        }

        return buffer;
    }

#ifdef BOOST_MSVC
# pragma warning(pop)
#endif

}}}} // Namespaces

boost::charconv::to_chars_result boost::charconv::to_chars(char* first, char* last, float value,
                                                           boost::charconv::chars_format fmt, int precision) noexcept
{
    return boost::charconv::detail::to_chars_float_impl(first, last, value, fmt, precision);
}

boost::charconv::to_chars_result boost::charconv::to_chars(char* first, char* last, double value,
                                                           boost::charconv::chars_format fmt, int precision) noexcept
{
    return boost::charconv::detail::to_chars_float_impl(first, last, value, fmt, precision);
}

#if BOOST_CHARCONV_LDBL_BITS == 64 || defined(BOOST_MSVC)

boost::charconv::to_chars_result boost::charconv::to_chars(char* first, char* last, long double value,
                                                           boost::charconv::chars_format fmt, int precision) noexcept
{
    return boost::charconv::detail::to_chars_float_impl(first, last, static_cast<double>(value), fmt, precision);
}

#elif (BOOST_CHARCONV_LDBL_BITS == 80 || BOOST_CHARCONV_LDBL_BITS == 128)

boost::charconv::to_chars_result boost::charconv::to_chars(char* first, char* last, long double value,
                                                           boost::charconv::chars_format fmt, int precision) noexcept
{
    static_assert(std::numeric_limits<long double>::is_iec559, "Long double must be IEEE 754 compliant");

    if (first > last)
    {
        return {last, std::errc::invalid_argument};
    }

    const auto classification = std::fpclassify(value);
    #if BOOST_CHARCONV_LDBL_BITS == 128
    if (classification == FP_NAN || classification == FP_INFINITE)
    {
        return boost::charconv::detail::to_chars_nonfinite(first, last, value, classification);
    }
    #else
    if (classification == FP_NAN || classification == FP_INFINITE)
    {
        const auto fd128 = boost::charconv::detail::ryu::long_double_to_fd128(value);
        const auto num_chars = boost::charconv::detail::ryu::generic_to_chars(fd128, first, last - first, fmt, precision);

        if (num_chars > 0)
        {
            return { first + num_chars, std::errc() };
        }
    }
    #endif

    if (fmt == boost::charconv::chars_format::general || fmt == boost::charconv::chars_format::scientific)
    {
        const auto fd128 = boost::charconv::detail::ryu::long_double_to_fd128(value);
        const auto num_chars = boost::charconv::detail::ryu::generic_to_chars(fd128, first, last - first, fmt, precision);

        if (num_chars > 0)
        {
            return { first + num_chars, std::errc() };
        }
    }
    else if (fmt == boost::charconv::chars_format::hex)
    {
        return boost::charconv::detail::to_chars_hex(first, last, value, precision);
    }
    else if (fmt == boost::charconv::chars_format::fixed)
    {
        const auto fd128 = boost::charconv::detail::ryu::long_double_to_fd128(value);
        const auto num_chars = boost::charconv::detail::ryu::generic_to_chars_fixed(fd128, first, last - first, precision);

        if (num_chars > 0)
        {
            return { first + num_chars, std::errc() };
        }
        else if (num_chars == -static_cast<int>(std::errc::result_out_of_range))
        {
            return { last, std::errc::result_out_of_range };
        }
    }

    // Fallback to printf methods
    return boost::charconv::detail::to_chars_printf_impl(first, last, value, fmt, precision);
}

#else

boost::charconv::to_chars_result boost::charconv::to_chars( char* first, char* last, long double value,
                                                            boost::charconv::chars_format fmt, int precision) noexcept
{
    if (std::isnan(value))
    {
        bool is_negative = false;
        if (std::signbit(value))
        {
            is_negative = true;
            *first++ = '-';
        }

        if (issignaling(value))
        {
            std::memcpy(first, "nan(snan)", 9);
            return { first + 9 + (int)is_negative, std::errc() };
        }
        else
        {
            if (is_negative)
            {
                std::memcpy(first, "nan(ind)", 8);
                return { first + 9, std::errc() };
            }
            else
            {
                std::memcpy(first, "nan", 3);
                return { first + 3, std::errc() };
            }
        }
    }

    // Fallback to printf
    return boost::charconv::detail::to_chars_printf_impl(first, last, value, fmt, precision);
}

#endif

#ifdef BOOST_CHARCONV_HAS_FLOAT128

boost::charconv::to_chars_result boost::charconv::to_chars(char* first, char* last, __float128 value, boost::charconv::chars_format fmt, int precision) noexcept
{
    char* const original_first = first;

    if (first > last)
    {
        return {last, std::errc::invalid_argument};
    }

    if (isnanq(value))
    {
        return boost::charconv::detail::to_chars_nonfinite(first, last, value, FP_NAN);
    }
    else if (isinfq(value))
    {
        return boost::charconv::detail::to_chars_nonfinite(first, last, value, FP_INFINITE);
    }

    if ((fmt == boost::charconv::chars_format::general || fmt == boost::charconv::chars_format::scientific))
    {
        const auto fd128 = boost::charconv::detail::ryu::float128_to_fd128(value);
        const auto num_chars = boost::charconv::detail::ryu::generic_to_chars(fd128, first, last - first, fmt, precision);

        if (num_chars > 0)
        {
            return { first + num_chars, std::errc() };
        }
    }
    else if (fmt == boost::charconv::chars_format::hex)
    {
        return boost::charconv::detail::to_chars_hex(first, last, value, precision);
    }
    else if (fmt == boost::charconv::chars_format::fixed)
    {
        const auto fd128 = boost::charconv::detail::ryu::float128_to_fd128(value);
        const auto num_chars = boost::charconv::detail::ryu::generic_to_chars_fixed(fd128, first, last - first, precision);

        if (num_chars > 0)
        {
            return { first + num_chars, std::errc() };
        }
        else if (num_chars == -static_cast<int>(std::errc::result_out_of_range))
        {
            return { last, std::errc::result_out_of_range };
        }
    }

    first = original_first;
    // Fallback to printf
    return boost::charconv::detail::to_chars_printf_impl(first, last, value, fmt, precision);
}

#endif

#ifdef BOOST_CHARCONV_HAS_FLOAT16
boost::charconv::to_chars_result boost::charconv::to_chars(char* first, char* last, std::float16_t value,
                                                           boost::charconv::chars_format fmt, int precision) noexcept
{
    return boost::charconv::detail::to_chars_float_impl(first, last, static_cast<float>(value), fmt, precision);
}
#endif

#ifdef BOOST_CHARCONV_HAS_FLOAT32
boost::charconv::to_chars_result boost::charconv::to_chars(char* first, char* last, std::float32_t value,
                                                           boost::charconv::chars_format fmt, int precision) noexcept
{
    static_assert(std::numeric_limits<std::float32_t>::digits == FLT_MANT_DIG &&
                  std::numeric_limits<std::float32_t>::min_exponent == FLT_MIN_EXP,
                  "float and std::float32_t are not the same layout like they should be");

    return boost::charconv::detail::to_chars_float_impl(first, last, static_cast<float>(value), fmt, precision);
}
#endif

#ifdef BOOST_CHARCONV_HAS_FLOAT64
boost::charconv::to_chars_result boost::charconv::to_chars(char* first, char* last, std::float64_t value,
                                                           boost::charconv::chars_format fmt, int precision) noexcept
{
    static_assert(std::numeric_limits<std::float64_t>::digits == DBL_MANT_DIG &&
                  std::numeric_limits<std::float64_t>::min_exponent == DBL_MIN_EXP,
                  "double and std::float64_t are not the same layout like they should be");
    
    return boost::charconv::detail::to_chars_float_impl(first, last, static_cast<double>(value), fmt, precision);
}
#endif

#if defined(BOOST_CHARCONV_HAS_STDFLOAT128) && defined(BOOST_CHARCONV_HAS_FLOAT128)
boost::charconv::to_chars_result boost::charconv::to_chars(char* first, char* last, std::float128_t value,
                                                           boost::charconv::chars_format fmt, int precision) noexcept
{
    return boost::charconv::to_chars(first, last, static_cast<__float128>(value), fmt, precision);
}
#endif

#ifdef BOOST_CHARCONV_HAS_BFLOAT16
boost::charconv::to_chars_result boost::charconv::to_chars(char* first, char* last, std::bfloat16_t value,
                                                           boost::charconv::chars_format fmt, int precision) noexcept
{
    return boost::charconv::detail::to_chars_float_impl(first, last, static_cast<float>(value), fmt, precision);
}
#endif
