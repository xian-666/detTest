#pragma once
#include<string>
//
// Created by xin on 2022/9/12.
//
namespace AC {
	class  DumpHelper
	{
	public:
		/// <summary>
		/// 设置是否记录崩溃转储
		/// 默认否
		/// </summary>
		/// <param name="value">是否记录崩溃转储</param>
		static void SetIsDumpCrash(bool value);
		/// <summary>
		/// 设置崩溃转储路径
		/// 默认为".\\"
		/// </summary>
		/// <param name="directory">崩溃转储路径</param>
		static void SetDumpDirectory(const std::string& directory);
		/// <summary>
		/// 崩溃转储文件数量
		/// 超过数量自动删除旧文件
		/// </summary>
		/// <param name="count">最大文件数量，默认500</param>
		static void SetDumpMaxFileCount(int count);
		/// <summary>
		/// 获取内存快照转储,可以任意时刻记录（包括非崩溃情况下）。
		/// </summary>
		/// <param name="path">保存文件名</param>
		static void Snapshot(const std::string& path);
	};
}
