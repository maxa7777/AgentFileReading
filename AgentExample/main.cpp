// producer-consumer-average.cpp
// compile with: /EHsc
#include <agents.h>
#include <iostream>
#include <string>
#include <fstream>
#include <chrono>
#include <thread>
#include <sstream>
#include <ppl.h>
#include <concurrent_vector.h>
#include "main.hpp"

using namespace concurrency;
using namespace std;
const string ReadFileName = "PointCloud.csv";
const string gSentinel = "AgentSenti";
struct lineInfo
{
	lineInfo( double x, double y, double z ) {
		X = x;
		Y = y;
		Z = z;
	}
	double X;
	double Y;
	double Z;
};

// A Semaphore type that uses cooperative blocking semantics.
class Semaphore
{
public:
	explicit Semaphore(long long capacity)
		: _semaphore_count(capacity)
	{
	}

	// Acquires access to the Semaphore.
	void acquire()
	{
		// The capacity of the Semaphore is exceeded when the Semaphore count 
		// falls below zero. When this happens, add the current context to the 
		// back of the wait queue and block the current context.
		if (--_semaphore_count < 0)
		{
			_waiting_contexts.push(Context::CurrentContext());
			Context::Block();
		}
	}

	// Releases access to the Semaphore.
	void release()
	{
		// If the Semaphore count is negative, unblock the first waiting context.
		if (++_semaphore_count <= 0)
		{
			// A call to acquire might have decremented the counter, but has not
			// yet finished adding the context to the queue. 
			// Create a spin loop that waits for the context to become available.
			Context* waiting = NULL;
			while (!_waiting_contexts.try_pop(waiting))
			{
				(Context::Yield)(); // <windows.h> defines Yield as a macro. The parenthesis around Yield prevent the macro expansion so that Context::Yield() is called.  
			}

			// Unblock the context.
			waiting->Unblock();
		}
	}

private:
	// The Semaphore count.
	atomic<long long> _semaphore_count;

	// A concurrency-safe queue of contexts that must wait to 
	// acquire the Semaphore.
	concurrent_queue<Context*> _waiting_contexts;
};


class FileRead_agent : public agent {
public:
	FileRead_agent( ITarget<shared_ptr<vector<string>>>& target,string path,int recieveCnt,string sentinel,shared_ptr<Semaphore> sem ) :_target( target ),_path(path),_recCnt(recieveCnt),_sentinel(sentinel),_semapho(sem) {};

protected:
	void run() {

		std::ifstream inputFile(_path);
		//終了条件を設定する
		auto endAnnounce = make_shared<vector<string>>();
		endAnnounce->push_back( _sentinel );
		if (!inputFile.is_open()) {
			asend( _target, endAnnounce );//終了条件としてendAnounceを投げる
			done();
		}

		std::string line;
		shared_ptr<vector<string>> buffers = make_shared<vector<string>>();
		buffers->reserve( BLOCK_COUNT );
		//
		while (std::getline(inputFile, line)) {
			buffers->push_back( line );
			//読み込んだ行がBLOCK_COUNTに達したらunbounded_bufferに送る
			if (buffers->size() == BLOCK_COUNT) {
				_semapho->acquire();
				send( _target, buffers );
				buffers.reset();//本スコープでは不要になったポインタを開放
				buffers = make_shared<vector<string>>();//作り直し
			}
		}
		//BLOCK_COUNTの端数を最後に送る
		if (buffers->size() > 0) {
			send( _target, buffers );
		}

		inputFile.close();//入力ファイルを閉じる
		//立ち上げたTargetに終了を通知する
		for (size_t i = 0; i < _recCnt; i++)
		{
			send( _target, endAnnounce );
		}
		done();//読込Agent終了
	}
private:
	const int BLOCK_COUNT = 1000000;
	ITarget<shared_ptr<vector<string>>>& _target;
	string _path;
	int _recCnt;
	string _sentinel;
	shared_ptr<Semaphore> _semapho;
};

class Converter_Agent :public agent {
public:
	Converter_Agent( ISource<shared_ptr<vector<string>>>& source,bool usePara,string sentinel,shared_ptr<Semaphore> sem ) :_source( source ),_usePara(usePara),_sentinel(sentinel),_semapho(sem){};
	concurrency::concurrent_vector<lineInfo> GetInfos()& { return _infos; }
	size_t GetInfoCounts() { return _infos.size(); }
protected:
	void run() {

		shared_ptr<vector<string>> buffers=receive(_source);
		_semapho->release();
		if (!buffers) {
			done();
			return;
		}
		while (buffers) {
			//EndAnounceが送られたら終了
			if (buffers->size() == 1 && _sentinel == buffers->at( 0 )) {
				done();
				return;
			}
			if (_usePara) {
				parallel_for_each( buffers->begin(), buffers->end(), [&]( string buf ) {
					std::vector<std::string> tokens;
					std::istringstream iss( buf );
					string token;
					while (std::getline( iss, token, ',' )) {
						tokens.push_back( token );
					}
					_infos.push_back( lineInfo( stod( tokens[0] ), stod( tokens[1] ), stod( tokens[2] ) ) );
				} );
			}
			else {
				for (auto& buf : *buffers) {
					//各行を','で分割し、Doubleに変換する
					std::vector<std::string> tokens;
					std::istringstream iss( buf );
					string token;
					while (std::getline( iss, token, ',' )) {
						tokens.push_back( token );
					}
					_infos.push_back( lineInfo( stod( tokens[0] ), stod( tokens[1] ), stod( tokens[2] ) ) );
				};
			}
			//次のブロックを読み込む
			buffers = receive( _source );
			_semapho->release();
		}
		done();
	}

private:
	ISource<shared_ptr<vector<string>>>& _source;
	bool _usePara;
	concurrency::concurrent_vector<lineInfo>_infos;
	string _sentinel;
	shared_ptr<Semaphore> _semapho;
};

concurrency::concurrent_vector<lineInfo>& SeekingGetInfos(string path) {
	//１行読んだら_targetに投げる
	std::ifstream inputFile(path);

	std::string line;
	concurrency::concurrent_vector<lineInfo> infos;
	infos.reserve( 1000000 );

	while (std::getline(inputFile, line)) {
		std::vector<std::string> tokens;
		std::istringstream iss(line);
		string token;
		while (std::getline(iss, token, ',')) {
			tokens.push_back(token);
		}
		infos.push_back( lineInfo( stod( tokens[0] ), stod( tokens[1] ), stod( tokens[2] ) ) );
	}
	return infos;
}

concurrency::concurrent_vector<lineInfo>& ReadWithAgent() {
	shared_ptr<Semaphore> semapho = make_shared<Semaphore>(1);
	unbounded_buffer<shared_ptr<vector<string>>> buffer;
	FileRead_agent reader( buffer, ReadFileName,1,gSentinel,semapho );
	Converter_Agent converter( buffer,false,gSentinel,semapho );

	reader.start();
	converter.start();

	agent::wait( &reader );
	agent::wait( &converter );

	auto infos = converter.GetInfos();
	return infos;
}

//１つの読み込みAgentと複数の変換Agentを立てる
void ReadWithMultiAgent(concurrency::concurrent_vector<lineInfo>& allInfos) {
	shared_ptr<Semaphore> semapho = make_shared<Semaphore>(4);
	//メッセージをやりとりするbufferを定義
	unbounded_buffer<shared_ptr<vector<string>>> buffer;
	FileRead_agent reader( buffer, ReadFileName,3,gSentinel,semapho );
	Converter_Agent converter( buffer,false,gSentinel,semapho );
	Converter_Agent converter2( buffer,false ,gSentinel,semapho);
	Converter_Agent converter3( buffer,false ,gSentinel,semapho);


	reader.start();
	converter.start();
	converter2.start();
	converter3.start();

	//すべてのAgentの終了を待機する
	agent::wait( &reader );
	agent::wait( &converter );
	agent::wait( &converter2 );
	agent::wait( &converter3 );

	//3つの変換Agentから結果を受取り、結合
	auto& moveInfos = converter.GetInfos();
	allInfos=std::move( moveInfos );
	auto& moveInfos_ = converter2.GetInfos();
	std::move( moveInfos_.begin(), moveInfos_.end(), std::back_inserter(allInfos) );
	auto& moveInfos3 = converter3.GetInfos();
	std::move( moveInfos3.begin(), moveInfos3.end(), std::back_inserter(allInfos) );
	cout << "AllInfos:" << allInfos.size() << endl;
}

void AgentMain() {
	std::chrono::system_clock::time_point  start, seek,agent,pAgent; // 型は auto で可
	start = std::chrono::system_clock::now(); // 計測開始時間
	ReadWithAgent();
	agent = std::chrono::system_clock::now();

	SeekingGetInfos(ReadFileName);
	seek = std::chrono::system_clock::now();

	concurrency::concurrent_vector<lineInfo> allInfos;
	ReadWithMultiAgent(allInfos);
	pAgent = std::chrono::system_clock::now();

	cout <<"Agent仕様："<< std::chrono::duration_cast<std::chrono::milliseconds>(agent - start).count() << endl;
	cout <<"連続処理："<< std::chrono::duration_cast<std::chrono::milliseconds>(seek - agent).count() << endl;
	cout <<"並列Agent仕様："<< std::chrono::duration_cast<std::chrono::milliseconds>(pAgent - seek).count() << endl;
}
int wmain()
{
	//semaphoMain();
	AgentMain();


	cin.get();
}