/*
 * Copyright 2011 Branimir Karadzic. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include <string.h>
#include "url.h"

void tokenizeUrl(const char* _url, char* _buf, size_t _bufSize, char* _tokens[UrlToken::Count])
{
	for (int ii = 0; ii < UrlToken::Count; ++ii)
	{
		_tokens[ii] = const_cast<char*>("");
	}

	if (NULL != _url)
	{
		_buf[0] = '\0';
		strncpy(_buf, _url, _bufSize);
		char* find = strstr(_buf, "://");

		if (NULL != find)
		{
			//scheme://username:password@host.com:80/this/is/path/index.php?query="value"#fragment
			*find = '\0';
			_tokens[UrlToken::Scheme] = _buf;
			_tokens[UrlToken::Host] = find + 3;

			find = strchr(_tokens[UrlToken::Host], '/');
			if (NULL != find)
			{
				*find = '\0';
				_tokens[UrlToken::Path] = find+1;
			}

			//username:password@host.com:80
			find = strchr(_tokens[UrlToken::Host], '@');
			if (NULL != find)
			{
				*find = '\0';
				_tokens[UrlToken::UserName] = _tokens[UrlToken::Host];
				_tokens[UrlToken::Host] = find+1;
			}

			//host.com:80
			find = strchr(_tokens[UrlToken::Host], ':');
			if (NULL != find)
			{
				*find = '\0';
				_tokens[UrlToken::Port] = find+1;
			}

			if (NULL != _tokens[UrlToken::UserName])
			{
				//username:password
				find = strchr(_tokens[UrlToken::UserName], ':');
				if (NULL != find)
				{
					*find = '\0';
					_tokens[UrlToken::Password] = find+1;
				}
			}

			if (NULL != _tokens[UrlToken::Path])
			{
				//this/is/path/index.php?query="value"#fragment
				find = strchr(_tokens[UrlToken::Path], '#');
				if (NULL != find)
				{
					*find = '\0';
					_tokens[UrlToken::Fragment] = find+1;
				}

				//this/is/path/index.php?query="value"
				find = strchr(_tokens[UrlToken::Path], '?');
				if (NULL != find)
				{
					*find = '\0';
					_tokens[UrlToken::Query] = find+1;
				}
			}
		}
	}
}