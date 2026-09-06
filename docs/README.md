# BLE工程师实战项目仓库
> 项目目标：完成一套BLE主机‑从机完整通信方案，用于求职面试作品集

## 硬件清单
- ESP32‑C3 SuperMini（BLE Central蓝牙主机）
- nRF52840开发板（BLE Peripheral传感器从机）
- USB串口、杜邦线若干

## 项目目标
1. nrf52840_sensor：从机端，采集传感器数据，通过BLE广播+GATT服务上报数据。
2. esp32c3_central：ESP32‑C3作为蓝牙主机，扫描、连接从机，读取GATT数据，业务逻辑处理。
3. pc_test_tool：PC端辅助测试脚本，用于验证BLE协议，辅助定位问题。

## 当前阶段
S0‑1：仓库目录结构搭建完成；环境验证完成，ESP‑IDF5.1.4编译烧录串口调试正常。

## 文档目录说明
- docs/notes：学习笔记、踩坑记录
- docs/captures：截图、抓包、现场测试证据
- docs/test‑report：测试用例与测试报告
- docs/architecture：架构设计、模块划分
