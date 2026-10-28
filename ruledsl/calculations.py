def bit_reverse(value, width):
    result = 0
    for index in range(width):
        result = (result << 1) | ((value >> index) & 1)
    return result


def mul_high_u(left, right, width):
    return (left * right) >> width
