---
title: tec::Command

---

# tec::Command



 [More...](#detailed-description)

## Public Functions

|                | Name           |
| -------------- | -------------- |
| | **[Command](/engine/Classes/structtec_1_1_command/#function-command)**(std::function< void(T *)> && command) |
| | **[Command](/engine/Classes/structtec_1_1_command/#function-command)**([Command](/engine/Classes/structtec_1_1_command/) && c) |

## Public Attributes

|                | Name           |
| -------------- | -------------- |
| std::function< void(T *)> | **[command](/engine/Classes/structtec_1_1_command/#variable-command)**  |

## Detailed Description

```cpp
template <class T >
struct tec::Command;
```

## Public Functions Documentation

### function Command

```cpp
inline Command(
    std::function< void(T *)> && command
)
```


### function Command

```cpp
inline Command(
    Command && c
)
```


## Public Attributes Documentation

### variable command

```cpp
std::function< void(T *)> command;
```


-------------------------------

Updated on  6 August 2021 at 01:15:52 UTC